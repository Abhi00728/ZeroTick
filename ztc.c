#include "zt_common.h"
#include <ctype.h>

// ============================================================================
// STAGE 1: LEXER (Turns .zs text into a flat list of Tokens)
// ============================================================================
typedef enum {
    TOK_EOF = 0,
    TOK_LET,        // "LET"
    TOK_IF,         // "IF"
    TOK_THEN,       // "THEN"
    TOK_BUY,        // "BUY" or "FIRE_BUY"
    TOK_SELL,       // "SELL" or "FIRE_SELL"
    TOK_KILL,       // "KILL_SWITCH"
    TOK_AT,         // "AT" or "PRICE"
    TOK_IDENT,      // Variable or packet field name (e.g., ask_price, spread)
    TOK_NUMBER,     // Integer literal (e.g., 64900)
    TOK_ASSIGN,     // '='
    TOK_PLUS,       // '+'
    TOK_MINUS,      // '-'
    TOK_MUL,        // '*'
    TOK_GT,         // '>'
    TOK_LT          // '<'
} token_type_t;

typedef struct {
    token_type_t type;
    char         lexeme[32];
    int32_t      num_val;
} token_t;

#define MAX_TOKENS 256
static token_t tokens[MAX_TOKENS];
static int num_tokens = 0;
static int cur_tok = 0;

void tokenize(const char *src) {
    int i = 0;
    num_tokens = 0;

    while (src[i] != '\0' && num_tokens < MAX_TOKENS - 1) {
        // Skip whitespace
        if (isspace((unsigned char)src[i])) {
            i++;
            continue;
        }
        // Skip '#' comments until end of line
        if (src[i] == '#') {
            while (src[i] != '\0' && src[i] != '\n') i++;
            continue;
        }
        // Single-character operators
        if (src[i] == '=') { tokens[num_tokens++] = (token_t){TOK_ASSIGN, "=", 0}; i++; continue; }
        if (src[i] == '+') { tokens[num_tokens++] = (token_t){TOK_PLUS,   "+", 0}; i++; continue; }
        if (src[i] == '-') { tokens[num_tokens++] = (token_t){TOK_MINUS,  "-", 0}; i++; continue; }
        if (src[i] == '*') { tokens[num_tokens++] = (token_t){TOK_MUL,    "*", 0}; i++; continue; }
        if (src[i] == '>') { tokens[num_tokens++] = (token_t){TOK_GT,     ">", 0}; i++; continue; }
        if (src[i] == '<') { tokens[num_tokens++] = (token_t){TOK_LT,     "<", 0}; i++; continue; }

        // Numbers
        if (isdigit((unsigned char)src[i])) {
            int32_t val = 0;
            int start = i;
            while (isdigit((unsigned char)src[i])) {
                val = val * 10 + (src[i] - '0');
                i++;
            }
            token_t t = {TOK_NUMBER, "", val};
            int len = i - start;
            if (len > 31) len = 31;
            memcpy(t.lexeme, src + start, len);
            t.lexeme[len] = '\0';
            tokens[num_tokens++] = t;
            continue;
        }

        // Identifiers and Keywords (allow letters, digits, underscores, dots)
        if (isalpha((unsigned char)src[i]) || src[i] == '_') {
            int start = i;
            while (isalnum((unsigned char)src[i]) || src[i] == '_' || src[i] == '.') i++;
            int len = i - start;
            if (len > 31) len = 31;

            token_t t = {TOK_IDENT, "", 0};
            memcpy(t.lexeme, src + start, len);
            t.lexeme[len] = '\0';

            // Strip optional "tick." prefix if written like "tick.ask_price"
            if (strncmp(t.lexeme, "tick.", 5) == 0) {
                memmove(t.lexeme, t.lexeme + 5, strlen(t.lexeme + 5) + 1);
            }

            if (strcmp(t.lexeme, "LET") == 0) t.type = TOK_LET;
            else if (strcmp(t.lexeme, "IF") == 0) t.type = TOK_IF;
            else if (strcmp(t.lexeme, "THEN") == 0) t.type = TOK_THEN;
            else if (strcmp(t.lexeme, "BUY") == 0 || strcmp(t.lexeme, "FIRE_BUY") == 0) t.type = TOK_BUY;
            else if (strcmp(t.lexeme, "SELL") == 0 || strcmp(t.lexeme, "FIRE_SELL") == 0) t.type = TOK_SELL;
            else if (strcmp(t.lexeme, "KILL_SWITCH") == 0) t.type = TOK_KILL;
            else if (strcmp(t.lexeme, "AT") == 0 || strcmp(t.lexeme, "PRICE") == 0) t.type = TOK_AT;
            else if (strcmp(t.lexeme, "QTY") == 0) continue; // Optional noise word

            tokens[num_tokens++] = t;
            continue;
        }

        fprintf(stderr, "[-] Lexer Error: Unexpected character '%c'\n", src[i]);
        exit(1);
    }
    tokens[num_tokens] = (token_t){TOK_EOF, "EOF", 0};
}

// ============================================================================
// STAGE 2: ABSTRACT SYNTAX TREE (AST) DEFINITIONS & PARSER
// ============================================================================
typedef enum {
    AST_NUM,        // Constant integer (e.g., 64900)
    AST_FIELD,      // Packet field (bid_price, ask_price, bid_vol, ask_vol)
    AST_VAR,        // User variable defined via LET (e.g., spread)
    AST_BINOP,      // Math or Comparison (+, -, *, >, <)
    AST_LET_STMT,   // LET var = expr
    AST_IF_STMT     // IF cond THEN action
} ast_type_t;

typedef struct ast_node {
    ast_type_t       type;
    token_type_t     op;          // For AST_BINOP (+, -, *, >, <) or Action (BUY/SELL/KILL)
    char             name[32];    // Variable or field name
    int32_t          num_val;     // Constant value or Order Qty
    uint8_t          field_id;    // FIELD_BID_PRICE, etc.
    struct ast_node *left;
    struct ast_node *right;
    struct ast_node *price_expr;  // Price expression for BUY/SELL
} ast_node_t;

#define MAX_AST_NODES 128
static ast_node_t ast_pool[MAX_AST_NODES];
static int ast_pool_idx = 0;

ast_node_t* alloc_ast(ast_type_t type) {
    if (ast_pool_idx >= MAX_AST_NODES) {
        fprintf(stderr, "[-] AST pool exhausted!\n");
        exit(1);
    }
    ast_node_t *n = &ast_pool[ast_pool_idx++];
    memset(n, 0, sizeof(ast_node_t));
    n->type = type;
    return n;
}

token_t* peek_tok(void) { return &tokens[cur_tok]; }
token_t* next_tok(void) { return &tokens[cur_tok++]; }

void expect_tok(token_type_t expected, const char *err_msg) {
    if (peek_tok()->type != expected) {
        fprintf(stderr, "[-] Syntax Error: Expected %s, but got '%s'\n", err_msg, peek_tok()->lexeme);
        exit(1);
    }
    next_tok();
}

int resolve_packet_field(const char *name) {
    if (strcmp(name, "bid_price") == 0) return FIELD_BID_PRICE;
    if (strcmp(name, "ask_price") == 0) return FIELD_ASK_PRICE;
    if (strcmp(name, "bid_vol")   == 0) return FIELD_BID_VOL;
    if (strcmp(name, "ask_vol")   == 0) return FIELD_ASK_VOL;
    return -1;
}

// Parses a single value: a number, a packet field, or a LET variable
ast_node_t* parse_primary(void) {
    token_t *t = next_tok();
    if (t->type == TOK_NUMBER) {
        ast_node_t *n = alloc_ast(AST_NUM);
        n->num_val = t->num_val;
        return n;
    }
    if (t->type == TOK_IDENT) {
        int fld = resolve_packet_field(t->lexeme);
        if (fld >= 0) {
            ast_node_t *n = alloc_ast(AST_FIELD);
            strncpy(n->name, t->lexeme, 31);
            n->field_id = (uint8_t)fld;
            return n;
        } else {
            ast_node_t *n = alloc_ast(AST_VAR);
            strncpy(n->name, t->lexeme, 31);
            return n;
        }
    }
    fprintf(stderr, "[-] Syntax Error: Expected number or variable, got '%s'\n", t->lexeme);
    exit(1);
}

// Parses arithmetic expressions: primary ( (+ | - | *) primary )*
ast_node_t* parse_math_expr(void) {
    ast_node_t *left = parse_primary();
    while (peek_tok()->type == TOK_PLUS || peek_tok()->type == TOK_MINUS || peek_tok()->type == TOK_MUL) {
        token_type_t op = next_tok()->type;
        ast_node_t *right = parse_primary();
        ast_node_t *bin = alloc_ast(AST_BINOP);
        bin->op = op;
        bin->left = left;
        bin->right = right;
        left = bin;
    }
    return left;
}

// Parses a program (list of LET statements followed by an IF statement)
#define MAX_STMTS 16
static ast_node_t* program_stmts[MAX_STMTS];
static int num_stmts = 0;

void parse_program(void) {
    while (peek_tok()->type != TOK_EOF && num_stmts < MAX_STMTS) {
        if (peek_tok()->type == TOK_LET) {
            next_tok(); // consume LET
            token_t *var_tok = next_tok();
            if (var_tok->type != TOK_IDENT) {
                fprintf(stderr, "[-] Syntax Error: Expected variable name after LET\n");
                exit(1);
            }
            expect_tok(TOK_ASSIGN, "'=' after variable name");
            ast_node_t *expr = parse_math_expr();

            ast_node_t *stmt = alloc_ast(AST_LET_STMT);
            strncpy(stmt->name, var_tok->lexeme, 31);
            stmt->left = expr;
            program_stmts[num_stmts++] = stmt;
        }
        else if (peek_tok()->type == TOK_IF) {
            next_tok(); // consume IF
            ast_node_t *lhs = parse_math_expr();

            token_t *cmp_tok = next_tok();
            if (cmp_tok->type != TOK_GT && cmp_tok->type != TOK_LT) {
                fprintf(stderr, "[-] Syntax Error: Expected '>' or '<' in IF condition\n");
                exit(1);
            }
            ast_node_t *rhs = parse_math_expr();

            ast_node_t *cond = alloc_ast(AST_BINOP);
            cond->op = cmp_tok->type;
            cond->left = lhs;
            cond->right = rhs;

            expect_tok(TOK_THEN, "'THEN' after IF condition");

            ast_node_t *if_stmt = alloc_ast(AST_IF_STMT);
            if_stmt->left = cond;

            token_t *act = next_tok();
            if (act->type == TOK_BUY || act->type == TOK_SELL) {
                if_stmt->op = act->type;
                token_t *qty_tok = next_tok();
                if (qty_tok->type != TOK_NUMBER) {
                    fprintf(stderr, "[-] Syntax Error: Expected quantity number after BUY/SELL\n");
                    exit(1);
                }
                if_stmt->num_val = qty_tok->num_val;

                // Optional "AT <field>" (defaults to ask_price for BUY, bid_price for SELL)
                if (peek_tok()->type == TOK_AT) {
                    next_tok(); // consume AT
                    if_stmt->price_expr = parse_primary();
                } else {
                    ast_node_t *def_px = alloc_ast(AST_FIELD);
                    if (act->type == TOK_BUY) {
                        strcpy(def_px->name, "ask_price");
                        def_px->field_id = FIELD_ASK_PRICE;
                    } else {
                        strcpy(def_px->name, "bid_price");
                        def_px->field_id = FIELD_BID_PRICE;
                    }
                    if_stmt->price_expr = def_px;
                }
            } else if (act->type == TOK_KILL) {
                if_stmt->op = TOK_KILL;
            } else {
                fprintf(stderr, "[-] Syntax Error: Expected BUY, SELL, or KILL_SWITCH after THEN\n");
                exit(1);
            }
            program_stmts[num_stmts++] = if_stmt;
        }
        else {
            fprintf(stderr, "[-] Syntax Error: Unexpected token '%s'\n", peek_tok()->lexeme);
            exit(1);
        }
    }
}

// ============================================================================
// STAGE 3: COMPILER OPTIMIZATION PASS (Constant Folding on the AST)
// ============================================================================
static int optimizations_applied = 0;

void optimize_ast(ast_node_t *node) {
    if (!node) return;
    optimize_ast(node->left);
    optimize_ast(node->right);

    if (node->type == AST_BINOP &&
        node->left && node->left->type == AST_NUM &&
        node->right && node->right->type == AST_NUM) {

        int32_t a = node->left->num_val;
        int32_t b = node->right->num_val;
        int32_t folded = 0;
        char op_char = '?';

        if (node->op == TOK_PLUS)       { folded = a + b; op_char = '+'; }
        else if (node->op == TOK_MINUS) { folded = a - b; op_char = '-'; }
        else if (node->op == TOK_MUL)   { folded = a * b; op_char = '*'; }
        else return; // Don't fold comparison operators here

        printf("  [OPT PASS] Constant Folding: (%d %c %d) ---> %d (Saved runtime ALU op!)\n",
               a, op_char, b, folded);

        node->type = AST_NUM;
        node->num_val = folded;
        node->left = NULL;
        node->right = NULL;
        optimizations_applied++;
    }
}

// ============================================================================
// STAGE 4 & 5: IR / BYTECODE GENERATOR & STATIC SAFETY VERIFIER
// ============================================================================
typedef struct {
    char    name[32];
    uint8_t reg_idx;
} var_binding_t;

static var_binding_t sym_table[8];
static int num_vars = 0;
static uint8_t next_free_reg = 0;

uint8_t alloc_reg(void) {
    if (next_free_reg >= 8) {
        fprintf(stderr, "[-] Static Verifier Error: Register spill! Strategy exceeds 8 hardware VM registers (R0..R7).\n");
        exit(1);
    }
    return next_free_reg++;
}

int find_var_reg(const char *name) {
    for (int i = 0; i < num_vars; i++) {
        if (strcmp(sym_table[i].name, name) == 0) return sym_table[i].reg_idx;
    }
    fprintf(stderr, "[-] Semantic Error: Undefined variable '%s'\n", name);
    exit(1);
}

void emit_inst(zt_strategy_slot_t *slot, uint8_t opcode, uint8_t dst, uint8_t src1, uint8_t src2, int32_t imm) {
    if (slot->num_instructions >= MAX_INSTRUCTIONS - 1) {
        fprintf(stderr, "[-] Static Verifier Error: Program exceeds MAX_INSTRUCTIONS (%d)\n", MAX_INSTRUCTIONS);
        exit(1);
    }
    slot->code[slot->num_instructions++] = (zt_instruction_t){opcode, dst, src1, src2, imm};
}

// Recursively emits instructions for an AST expression and returns the Register holding the result
uint8_t codegen_expr(ast_node_t *node, zt_strategy_slot_t *slot) {
    if (node->type == AST_NUM) {
        uint8_t r = alloc_reg();
        emit_inst(slot, OP_LOAD_IMM, r, 0, 0, node->num_val);
        return r;
    }
    if (node->type == AST_FIELD) {
        uint8_t r = alloc_reg();
        emit_inst(slot, OP_LOAD_FIELD, r, node->field_id, 0, 0);
        return r;
    }
    if (node->type == AST_VAR) {
        return (uint8_t)find_var_reg(node->name);
    }
    if (node->type == AST_BINOP) {
        uint8_t r_left  = codegen_expr(node->left, slot);
        uint8_t r_right = codegen_expr(node->right, slot);
        uint8_t r_dst   = alloc_reg();

        if (node->op == TOK_PLUS)       emit_inst(slot, OP_ADD,    r_dst, r_left, r_right, 0);
        else if (node->op == TOK_MINUS) emit_inst(slot, OP_SUB,    r_dst, r_left, r_right, 0);
        else if (node->op == TOK_GT)    emit_inst(slot, OP_CMP_GT, r_dst, r_left, r_right, 0);
        else if (node->op == TOK_LT)    emit_inst(slot, OP_CMP_LT, r_dst, r_left, r_right, 0);
        else {
            fprintf(stderr, "[-] Verifier Error: Runtime '*' not allowed; use constant expressions.\n");
            exit(1);
        }
        return r_dst;
    }
    return 0;
}

void print_disassembly(const zt_strategy_slot_t *slot) {
    static const char *op_names[] = {
        "OP_HALT", "OP_LOAD_FIELD", "OP_LOAD_IMM", "OP_ADD", "OP_SUB",
        "OP_CMP_GT", "OP_CMP_LT", "OP_JMP_IF_ZERO", "OP_FIRE_BUY", "OP_FIRE_SELL", "OP_KILL_SWITCH"
    };
    static const char *field_names[] = {"bid_price", "ask_price", "bid_vol", "ask_vol"};

    printf("\n  [IR / BYTECODE DISASSEMBLY (%u instructions)]\n", slot->num_instructions);
    printf("  ------------------------------------------------------------------\n");
    for (uint32_t i = 0; i < slot->num_instructions; i++) {
        zt_instruction_t in = slot->code[i];
        printf("  code[%02u] | %-14s | dst=R%u | src1=%u | src2=%u | imm=%d",
               i, op_names[in.opcode], in.dst, in.src1, in.src2, in.imm_val);

        if (in.opcode == OP_LOAD_FIELD) printf("   ; R%u = tick.%s", in.dst, field_names[in.src1]);
        else if (in.opcode == OP_LOAD_IMM) printf("   ; R%u = %d", in.dst, in.imm_val);
        else if (in.opcode == OP_ADD) printf("   ; R%u = R%u + R%u", in.dst, in.src1, in.src2);
        else if (in.opcode == OP_SUB) printf("   ; R%u = R%u - R%u", in.dst, in.src1, in.src2);
        else if (in.opcode == OP_CMP_GT) printf("   ; R%u = (R%u > R%u)", in.dst, in.src1, in.src2);
        else if (in.opcode == OP_CMP_LT) printf("   ; R%u = (R%u < R%u)", in.dst, in.src1, in.src2);
        else if (in.opcode == OP_JMP_IF_ZERO) printf("   ; if (R%u == 0) HALT", in.src1);
        else if (in.opcode == OP_FIRE_BUY) printf("   ; BUY %d @ R%u", in.imm_val, in.src1);
        else if (in.opcode == OP_FIRE_SELL) printf("   ; SELL %d @ R%u", in.imm_val, in.src1);
        else if (in.opcode == OP_KILL_SWITCH) printf("   ; TRIP RISK KILL_SWITCH!");
        printf("\n");
    }
    printf("  ------------------------------------------------------------------\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <strategy_file.zs>\n", argv[0]);
        return 1;
    }

    // 1. Read .zs file from disk
    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        perror("[-] Failed to open .zs strategy file");
        return 1;
    }
    char source_buf[4096] = {0};
    size_t nread = fread(source_buf, 1, sizeof(source_buf) - 1, fp);
    fclose(fp);
    if (nread == 0) {
        fprintf(stderr, "[-] Strategy file is empty!\n");
        return 1;
    }

    printf("==================================================================\n");
    printf("  ZEROTICK DSL COMPILER & STATIC VERIFIER (ztc)\n");
    printf("  Source File: %s\n", argv[1]);
    printf("==================================================================\n");

    // 2. Run Stage 1 (Lexer) & Stage 2 (AST Parser)
    tokenize(source_buf);
    printf("[1/5] Lexer:      Tokenized %d tokens successfully.\n", num_tokens);

    parse_program();
    printf("[2/5] AST Parser: Built %d AST nodes across %d statements.\n", ast_pool_idx, num_stmts);

    // 3. Run Stage 3 (Constant Folding Optimization Pass)
    printf("[3/5] Optimizer:  Running Constant Folding pass on AST...\n");
    for (int i = 0; i < num_stmts; i++) {
        optimize_ast(program_stmts[i]);
    }
    if (optimizations_applied == 0) {
        printf("  [OPT PASS] No constant sub-expressions needed folding.\n");
    }

    // 4. Run Stage 4 & 5 (Codegen into a temporary strategy slot + Static Verification)
    zt_strategy_slot_t compiled_slot = {0};
    const char *base_name = strrchr(argv[1], '/');
    base_name = base_name ? (base_name + 1) : argv[1];
    strncpy(compiled_slot.strategy_name, base_name, sizeof(compiled_slot.strategy_name) - 1);

    for (int i = 0; i < num_stmts; i++) {
        ast_node_t *st = program_stmts[i];
        if (st->type == AST_LET_STMT) {
            uint8_t r = codegen_expr(st->left, &compiled_slot);
            memcpy(sym_table[num_vars].name, st->name, 31);
            sym_table[num_vars].reg_idx = r;
            num_vars++;
        } else if (st->type == AST_IF_STMT) {
            uint8_t cond_reg = codegen_expr(st->left, &compiled_slot);
            emit_inst(&compiled_slot, OP_JMP_IF_ZERO, 0, cond_reg, 0, 0);

            if (st->op == TOK_BUY || st->op == TOK_SELL) {
                uint8_t px_reg = codegen_expr(st->price_expr, &compiled_slot);
                uint8_t op = (st->op == TOK_BUY) ? OP_FIRE_BUY : OP_FIRE_SELL;
                emit_inst(&compiled_slot, op, 0, px_reg, 0, st->num_val);
            } else if (st->op == TOK_KILL) {
                emit_inst(&compiled_slot, OP_KILL_SWITCH, 0, 0, 0, 0);
            }
        }
    }
    emit_inst(&compiled_slot, OP_HALT, 0, 0, 0, 0);

    printf("[4/5] Verifier:   PASSED! Registers used: %u/8 | Instructions: %u/%d | Loops: 0 (Acyclic)\n",
           next_free_reg, compiled_slot.num_instructions, MAX_INSTRUCTIONS);

    print_disassembly(&compiled_slot);

    // 5. Connect to Shared Whiteboard & Perform Atomic Double-Buffer Hot-Swap!
    zt_whiteboard_t *wb = zt_open_whiteboard(true);
    if (!wb) {
        perror("[-] Failed to open Shared Whiteboard");
        return 1;
    }

    uint32_t current_active = atomic_load(&wb->active_slot);
    uint32_t target_slot    = 1 - current_active; // Write to the INACTIVE slot (0 -> 1, or 1 -> 0)

    // Copy compiled structs into inactive slot first
    wb->slots[target_slot] = compiled_slot;

    // Flip the active_slot pointer in a SINGLE atomic CPU instruction!
    atomic_store_explicit(&wb->active_slot, target_slot, memory_order_release);

    printf("[5/5] Hot-Swap:   SUCCESS! Injected '%s' into Slot %u (was Slot %u) with ZERO downtime!\n",
           compiled_slot.strategy_name, target_slot, current_active);
    printf("==================================================================\n");

    return 0;
}

# Strategy 2: Pre-Trade Risk Kill-Switch when Bid-Ask Spread blows out > 1000
LET spread = ask_price - bid_price
IF spread > 500 * 2 THEN KILL_SWITCH

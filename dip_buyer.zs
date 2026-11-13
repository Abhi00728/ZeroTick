# Strategy 3: Buy 50 units when Bid Volume exceeds Ask Volume by more than 80
LET vol_imbalance = bid_vol - ask_vol
IF vol_imbalance > 40 + 40 THEN BUY 50 AT ask_price

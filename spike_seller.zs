# Strategy 1: Sell into sudden price spikes above 65200
# Notice "65000 + 200" -> ztc will Constant-Fold this into 65200 at compile time!
IF bid_price > 65000 + 200 THEN SELL 25 AT bid_price

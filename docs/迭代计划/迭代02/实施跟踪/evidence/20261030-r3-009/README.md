# R3-009 targeted evidence

The public `TraderAdapter::getOrders` declaration documents its argument as a standardized contract code. Existing WtCore behavior also supports exact bare raw-code queries because orders are stored with the contract raw code. The implementation now preserves that exact match and additionally derives each order's standardized code from contract metadata; the derived code includes the exchange. If an order lacks contract metadata, the adapter attempts lookup by raw code plus exchange.

The cross-exchange test uses the same synthetic raw code (`ag2512`) for SHFE and DCE. It verifies each canonical standard-code query returns only the matching exchange, the bare raw query retains existing behavior and returns both matches, and empty query still returns all orders.

Build command:

```sh
/opt/homebrew/bin/cmake --build build-cov --target WtCore CoverageTests_wtcore -j4
```

Build exit status: 0. See `build.log`.

- `R3TraderTest.position_and_order_tables_read_seeded_state` — passed; see `standard-query.log` and XML.
- `R3TraderTest.get_orders_supports_standard_and_bare_codes_across_exchanges` — passed; see `raw-cross-exchange.log` and XML.

This is local fake-API evidence only; it does not validate provider order metadata quality or live trading behavior.

Related compatibility check: `P11SyncReport.order_report_rebuilds_table_and_undone` was also run and failed. Its assertion at `p11_sync_report_test.cpp:308` explicitly expects standardized-code lookup to return zero, which contradicts the public `getOrders(stdCode)` declaration and the R3-009 contract; it is stale under this fix. A second assertion at line 329 expects a repeated `onRspOrders` response to remove a prior order, but the current `onRspOrders` implementation reuses `_orders` and only adds/replaces IDs, so that failure is separate from this query change and outside the authorized R3 test scope. No P11 test or source outside R3 was modified.

P11 assertion follow-up: only the first, contract-conflicting std-code query assertion was updated in `p11_sync_report_test.cpp`. It now expects both live SHFE orders by canonical code, verifies local IDs 1001/1002 map to the original objects, and releases the returned map. `CoverageTests_wtcore` rebuilt successfully (`p11-rebuild.log`). The rerun passes that assertion and retains only the later `after->size()` failure: expected 1, actual 2 (`p11-after-standard-assertion.log` / XML). The replacement-versus-incremental contract for a repeated `onRspOrders` response is not established here; no product change or assertion change was made for it.

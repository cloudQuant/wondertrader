#!/usr/bin/env python3
"""Generate per-case lists for i03_traders_other_20261104s5.
usage: make_cases.py <bin_dir> <variant> <out_dir>
Writes <out_dir>/cases-<group>-<variant>.json in the run_cases.py format of
i03_leaks_main_20261104 (one gtest process per case, fixed timeout)."""
import json, os, sys

bin_dir, variant, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out_dir, exist_ok=True)

def b(mod):
    return os.path.join(bin_dir, "CoverageTests_t_Trader" + mod)

groups = {
    # T4-002 / I03-NEW-016 originally targeted Femas cases (leaks_main list) + lane.
    "t4002": [("Femas", n) for n in [
        "FemasContractFixture.position_query_response_appends_the_reported_item",
        "FemasContractFixture.position_query_unknown_contract_returns_empty_final_array",
        "FemasContractFixture.make_order_info_maps_vendor_order_fields",
        "FemasContractFixture.make_order_info_falls_back_to_local_id_and_maps_fok",
        "FemasContractFixture.order_query_and_push_callbacks",
        "FemasContractFixture.trade_query_response_reports_appended_records",
        "FemasContractFixture.i03_new016_destructor_returns_query_container_ownership_once",
    ]],
    # T6-009 originally targeted Mocker cases + t6_init WtCore case + R60 lifecycle (R60-003).
    "t6009": [("Mocker", n) for n in [
        "T6009Mocker.PendingAndCanceledOrderRefsReturnedExactlyOnce",
        "T6009Mocker.DestructorAloneReturnsPendingOrderRefs",
        "T6009Mocker.ReleaseFromWorkerCallbackDefersJoin",
        "T6009Mocker.DeleteTraderFromWorkerCallbackQuarantines",
        "T6009Mocker.RepeatedInitRejected",
        "T6009Mocker.ConcurrentExternalReleaseAndRequests",
        "R113MockerPartialCancel.R113Mocker_CancelCopiesOrderIdBeforeFinalActionRelease",
        "R113MockerPartialCancel.R113Mocker_PartialFillThenCancelPushesCanceled",
        "NEW020UdpAlignment.CompleteTickAcceptedAndTruncatedOrWrongTypeIgnored",
        "t6_init.wtcore_init_ext_rejects_tradermocker_null_config",
    ]],
    "r60003": [("Mocker", "R60MockerTest." + n) for n in [
        "same_instance_rejects_reinit_and_fresh_instances_deliver_each_connect",
        "released_instance_rejects_reinit_and_old_query_never_reaches_new_lifecycle",
        "release_reentered_from_connect_callback_does_not_terminate",
        "release_and_destructor_free_bound_udp_port",
        "repeated_instance_lifecycle_does_not_grow_process_fds",
        "ready_requests_remain_deferred_and_run_after_connect",
        "stopped_requests_are_rejected_without_retaining",
        "unexecuted_order_requests_release_references_on_destruction",
        "stopping_closes_request_admission_before_join",
        "duplicate_login_preserves_running_worker",
        "duplicate_connect_preserves_running_worker",
    ]],
    "r60001": [("Mocker", "R60SessionIsolation." + n) for n in [
        "async_connect_login_then_immediate_release_delivers_nothing_afterwards",
        "release_from_rsp_entrust_suppresses_rest_of_handler",
        "release_from_matcher_push_suppresses_rest_of_pass",
        "disconnect_ends_session_like_release_and_does_not_grow_fds",
    ]],
    "t4003": [("Femas", "T4003FemasLifecycle." + n) for n in [
        "release_joins_worker_and_delete_does_not_terminate",
        "idle_worker_still_executes_later_query",
        "release_waits_for_inflight_request_before_api_release",
        "release_waits_for_inflight_order_insert_before_api_release",
        "release_from_callback_defers_and_external_release_completes",
        "delete_trader_from_callback_quarantines",
        "admission_closed_after_release_and_repeated_connect_rejected",
        "late_callback_after_release_is_not_delivered",
        "callback_drain_timeout_reports_unquiesced_and_remover_quarantines",
    ]],
    "r93006": [("YD", "R93YdTest." + n) for n in [
        "open_cover_order_freezes_yesterday_available",
        "close_today_order_freezes_and_releases_today_available",
        "r93_mixed_yesterday_today_zero_fill_cancel_restores_origin",
        "r93_two_orders_zero_fill_cancels_restore_their_own_sources",
    ]],
    # Round 2 (2026-11-04 main-thread follow-up).
    "ydlife": [("YD", "YdDestroyLifecycle." + n) for n in [
        "release_waits_for_after_api_destroy_notification",
        "worker_joined_and_delete_does_not_terminate",
        "missing_after_destroy_notification_keeps_instance_and_remover_quarantines",
        "release_from_callback_defers_destroy_to_external_release",
        "delete_trader_from_callback_quarantines",
        "admission_closed_after_release_and_repeated_connect_rejected",
        "late_callback_after_release_is_not_delivered",
        "release_drains_concurrent_callbacks_before_destroy",
    ]] + [("YD", "R93YdTest.r93_mixed_close_trade_reduces_yesterday_then_today")],
    "xtps5": [("XTP", "XtpS5Lifecycle." + n) for n in [
        "error_callbacks_after_register_spi_null_do_not_crash",
        "delete_trader_from_sdk_callback_quarantines",
        "release_waits_for_inflight_callback_before_api_release",
        "callback_drain_timeout_retains_api_and_remover_quarantines",
        "late_disconnect_of_replaced_api_session_does_not_relogin",
        "disconnect_of_current_session_still_relogins",
        "init_fails_without_module_and_rejects_repeated_init",
    ]],
    "r214": [("Mocker", "R214MockerBranches.UnregisteredOrderDoesNotTradeOnOtherContractTick")],
    "lane": [("Femas", "*"), ("Mocker", "*"), ("YD", "*"), ("XTP", "*")],
}

for g, items in groups.items():
    cases = []
    for mod, name in items:
        cases.append({"id": "%s-%s" % (g, variant), "name": (mod + "-ALL") if name == "*" else name,
                      "bin": b(mod), "filter": name, "timeout": 300 if name == "*" else 60})
    json.dump(cases, open(os.path.join(out_dir, "cases-%s-%s.json" % (g, variant)), "w"), indent=1)
print("ok")

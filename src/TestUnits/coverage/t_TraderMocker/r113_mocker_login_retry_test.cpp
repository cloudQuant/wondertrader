/**
 * r113_mocker_login_retry_test.cpp  (lane R113 · wave19)
 *
 * 靶子：TraderMocker 生命周期接口的重复调用（缺陷留档，不启用）
 *   - connect 第二次：_thrd_worker.reset(new StdThread(...)) 销毁仍 joinable 的线程
 *   - login   第二次：_thrd_match.reset(new StdThread(...)) 同上
 *   - release 之后再 login：_terminated 已为 true，release 的早退导致新线程无人 join
 *
 * TraderMocker.cpp:640-657（release）、689-703（connect）、723-743（login）
 * 三条路径都会以 std::terminate 结束进程，属崩溃类，保留 DISABLED_ 占位；
 * 缺陷记录见 docs/迭代计划/迭代01/bugs/R113.md BUG-R113-001。
 */
#include "r113_mocker_common.h"

using R113MockerLoginRetry = r113m::MockerTest;

TEST_F(R113MockerLoginRetry, DISABLED_SecondLoginOrConnectTerminates)
{
	init_api(9815);
	connect_api();
	login_api();

	/* 第二次 login：_thrd_match 已 joinable，reset 会调用 std::terminate */
	api_.login("", "", "");

	/* 第二次 connect：_thrd_worker 已 joinable，同上 */
	api_.connect();
}

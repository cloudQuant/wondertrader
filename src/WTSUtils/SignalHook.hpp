/*!
 * \file SignalHook.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 信号处理钩子模块
 *
 * 本文件实现了对系统信号的捕获和处理机制，包括崩溃信号、终止信号等。
 * 提供了信号处理函数和安装信号钩子的功能，支持Windows和Linux平台。
 * 可以通过自定义回调函数来处理系统发出的各种信号，并在需要时输出堆栈跟踪信息。
 */
#pragma once
#include <signal.h>
#include "./StackTracer/StackTracer.h"

/**
 * @brief 信号日志回调函数
 * 
 * 全局变量，用于处理信号发生时的日志记录
 * 由install_signal_hooks函数设置，由handle_signal函数调用
 */
TracerLogCallback g_cbSignalLog = NULL;

/**
 * @brief 信号退出处理函数
 * 
 * 全局变量，用于处理信号发生时的自定义退出操作
 * 如果设置了此函数，则优先使用此函数处理退出操作，而不是简单调用exit()
 */
ExitHandler g_exitHandler = NULL;

/**
 * @brief 处理系统信号的函数
 * 
 * 此函数是处理各种系统信号的回调函数，在信号发生时被调用
 * 根据不同的信号类型执行不同的处理操作，包括日志记录、堆栈跟踪和退出处理
 * 对于Windows和Linux系统提供了不同的信号处理逻辑
 * 
 * @param signum 信号编号，指示发生的系统信号类型
 */
void handle_signal(int signum)
{
	static char buf[64] = { 0 };
	memset(buf, 0, 64);
	switch (signum)
	{
#ifdef _WIN32
	case SIGINT:          // interrupt
	case SIGBREAK:        // Ctrl-Break sequence
		g_cbSignalLog("app interrupted");
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGTERM:         // Software termination signal from kill
		g_cbSignalLog("app terminated");
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGILL:          // illegal instruction - invalid function image
	case SIGFPE:          // floating point exception
	case SIGSEGV:         // segment violation
	case SIGABRT:         // abnormal termination triggered by abort call
	case SIGABRT_COMPAT:  // SIGABRT compatible with other platforms, same as SIGABRT
		sprintf(buf, "app stopped by signal %d", signum);
		g_cbSignalLog(buf);
		print_stack_trace(g_cbSignalLog);
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
#else
	case SIGURG:       // discard signal       urgent condition present on socket
	case SIGCONT:      // discard signal       continue after stop
	case SIGCHLD:      // discard signal       child status has changed
	case SIGIO:        // discard signal       I/O is possible on a descriptor (see fcntl(2))
	case SIGWINCH:     // discard signal       Window size change
		sprintf(buf, "app discard signal %d", signum);
		g_cbSignalLog(buf);
		break;
	case SIGSTOP:      // stop process         stop (cannot be caught or ignored)
	case SIGTSTP:      // stop process         stop signal generated from keyboard
	case SIGTTIN:      // stop process         background read attempted from control terminal
	case SIGTTOU:      // stop process         background write attempted to control terminal
		sprintf(buf, "app stopped by signal %d", signum);
		g_cbSignalLog(buf);
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGINT:       // terminate process    interrupt program
		g_cbSignalLog("app interrupted");
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGTERM:      // terminate process    software termination signal
		g_cbSignalLog("app terminated");
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGKILL:      // terminate process    kill program
		g_cbSignalLog("app killed");
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGHUP:       // terminate process    terminal line hangup
		g_cbSignalLog("app has received SIGHUP");
		break;
	case SIGPIPE:      // terminate process    write on a pipe with no reader
	case SIGALRM:      // terminate process    real-time timer expired
	case SIGXCPU:      // terminate process    cpu time limit exceeded (see setrlimit(2))
	case SIGXFSZ:      // terminate process    file size limit exceeded (see setrlimit(2))
	case SIGVTALRM:    // terminate process    virtual time alarm (see setitimer(2))
	case SIGPROF:      // terminate process    profiling timer alarm (see setitimer(2))
		sprintf(buf, "app terminated by signal %d", signum);
		g_cbSignalLog(buf);
		print_stack_trace(g_cbSignalLog);
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGUSR1:      // terminate process    User defined signal 1
	case SIGUSR2:      // terminate process    User defined signal 2
		sprintf(buf, "app caught user defined signal %d", signum);
		g_cbSignalLog(buf);
		print_stack_trace(g_cbSignalLog);
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGQUIT:      // create core image    quit program
	case SIGILL:       // create core image    illegal instruction
	case SIGTRAP:      // create core image    trace trap
	case SIGABRT:      // create core image    abort program (formerly SIGIOT)
	case SIGFPE:       // create core image    floating-point exception
	case SIGBUS:       // create core image    bus error
		g_cbSignalLog("bus error");
		print_stack_trace(g_cbSignalLog);
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGSEGV:      // create core image    segmentation violation
		g_cbSignalLog("segmentation violation");
		print_stack_trace(g_cbSignalLog);
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
	case SIGSYS:       // create core image    non-existent system call invoked
		sprintf(buf, "app caught unexpected signal %d", signum);
		g_cbSignalLog(buf);
		print_stack_trace(g_cbSignalLog);
		if (g_exitHandler)
			g_exitHandler(signum);
		else
			exit(signum);
		break;
#endif // _WINDOWS
	default:
		sprintf(buf, "app caught unknown signal %d, signal ignored", signum);
		g_cbSignalLog(buf);
		break;
	}
}

/**
 * @brief 安装系统信号处理钩子
 * 
 * 此函数用于设置信号处理机制，将所有系统信号关联到handle_signal函数
 * 设置日志记录回调函数和可选的退出处理函数
 * 调用此函数后，程序将能够捕获并处理各种系统信号
 * 
 * @param cbLog 必需的日志回调函数，用于记录信号相关的日志信息
 * @param sigHandler 可选的信号退出处理函数，如果提供，将在用于处理需要终止程序的信号
 */
void install_signal_hooks(TracerLogCallback cbLog, ExitHandler sigHandler = NULL)
{
	g_cbSignalLog = cbLog;
	g_exitHandler = sigHandler;
	for (int s = 1; s < NSIG; s++)
	{
		signal(s, handle_signal);
	}
}


﻿/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

// This sample C application for Azure Sphere sends messages to, and receives
// responses from, a real-time capable application. It sends a message every
// second and prints the message which was sent, and the response which was received.
//
// It uses the following Azure Sphere libraries
// - log (messages shown in Visual Studio's Device Output window during debugging);
// - application (establish a connection with a real-time capable application).
// - eventloop (system invokes handlers for timer events)

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/socket.h>

#include <applibs/log.h>
#include <applibs/application.h>

#include "eventloop_timer_utilities.h"

#define RECV_BUFF_SIZE 32

/// <summary>
/// Exit codes for this application. These are used for the
/// application exit code. They must all be between zero and 255,
/// where zero is reserved for successful termination.
/// </summary>
typedef enum {
    ExitCode_Success = 0,
    ExitCode_TermHandler_SigTerm = 1,
    ExitCode_TimerHandler_Consume = 2,
    ExitCode_SendMsg_Send = 3,
    ExitCode_SocketHandler_Recv = 4,
    ExitCode_Init_EventLoop = 5,
    ExitCode_Init_SendTimer = 6,
    ExitCode_Init_Connection = 7,
    ExitCode_Init_SetSockOpt = 8,
    ExitCode_Init_RegisterIo = 9,
    ExitCode_Main_EventLoopFail = 10,
    ExitCode_Main_EventLoopSimReboot = 11
} ExitCode;

static int sockFd = -1;
static EventLoop *eventLoop = NULL;
static EventLoopTimer *sendTimer = NULL;
static EventRegistration *socketEventReg = NULL;
static volatile sig_atomic_t exitCode = ExitCode_Success;

static const char rtAppComponentId[] = "005180bc-402f-4cb3-a662-72937dbcde47";

static void TerminationHandler(int signalNumber);
static void SendTimerEventHandler(EventLoopTimer *timer);
static void SendMessageToRTApp(void);
static void SocketEventHandler(EventLoop *el, int fd, EventLoop_IoEvents events, void *context);
static void InitSigterm(void);
static ExitCode InitHandlers(void);
static void CloseHandlers(void);

/// <summary>
///     Signal handler for termination requests. This handler must be async-signal-safe.
/// </summary>
static void TerminationHandler(int signalNumber)
{
    // Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
    exitCode = ExitCode_TermHandler_SigTerm;
}

/// <summary>
///     Handle send timer event by writing data to the real-time capable application.
/// </summary>
static void SendTimerEventHandler(EventLoopTimer *timer)
{
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        exitCode = ExitCode_TimerHandler_Consume;
        return;
    }

    SendMessageToRTApp();
}

/// <summary>
///     Helper function for TimerEventHandler sends message to real-time capable application.
/// </summary>
static void SendMessageToRTApp(void)
{
    // Send "hl-app-to-rt-app-%02d" message to RTApp, where the number cycles from 00 to 99.
    static int iter = 0;

    static char txMessage[48];
    snprintf(txMessage, sizeof(txMessage), "hl-app-to-rt-app-adding%02d", iter);
    iter = (iter + 1) % 100;
    Log_Debug("Sending: %s\n", txMessage);

    int bytesSent = send(sockFd, txMessage, strlen(txMessage), 0);
    if (bytesSent == -1) {
        Log_Debug("ERROR: Unable to send message: %d (%s)\n", errno, strerror(errno));
        exitCode = ExitCode_SendMsg_Send;
        return;
    }
}

static bool MsgParseIsReboot(char rxBuf[RECV_BUFF_SIZE], int len)
{
    if (!rxBuf || len <= 0) {
        return false;
    }

    return (strncmp(rxBuf, "reboot!!", (size_t)len) == 0);
}

/// <summary>
///     Handle socket event by reading incoming data from real-time capable application.
/// </summary>
static void SocketEventHandler(EventLoop *el, int fd, EventLoop_IoEvents events, void *context)
{
    // Read response from real-time capable application.
    // If the RTApp has sent more than 32 bytes, then truncate.
    char rxBuf[RECV_BUFF_SIZE];
    int bytesReceived = recv(fd, rxBuf, sizeof(rxBuf), 0);
    Log_Debug("SocketEventHandler\n");

    if (bytesReceived == -1) {
        Log_Debug("ERROR: Unable to receive message: %d (%s)\n", errno, strerror(errno));
        exitCode = ExitCode_SocketHandler_Recv;
        return;
    }

    Log_Debug("Received %d bytes: ", bytesReceived);
    for (int i = 0; i < bytesReceived; ++i) {
        Log_Debug("%c", isprint(rxBuf[i]) ? rxBuf[i] : '.');
    }
    Log_Debug("\n");

    if (MsgParseIsReboot(rxBuf, bytesReceived)) {
        Log_Debug("Simulated reboot cmd received\n");
        exitCode = ExitCode_Main_EventLoopSimReboot;
    }
}


/// <summary>
///     Set up SIGTERM termination handler and event handlers for send timer
/// </summary>
static void InitSigterm(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);
}

/// <summary>
///     Set up SIGTERM termination handler and event handlers for send timer
///     and to receive data from real-time capable application.
/// </summary>
/// <returns>
///     ExitCode_Success if all resources were allocated successfully; otherwise another
///     ExitCode value which indicates the specific failure.
/// </returns>
static ExitCode InitHandlers(void)
{
    eventLoop = EventLoop_Create();
    if (eventLoop == NULL) {
        Log_Debug("Could not create event loop.\n");
        return ExitCode_Init_EventLoop;
    }

    // Register a one second timer to send a message to the RTApp.
    static const struct timespec sendPeriod = {.tv_sec = 1, .tv_nsec = 0};
    sendTimer = CreateEventLoopPeriodicTimer(eventLoop, &SendTimerEventHandler, &sendPeriod);
    if (sendTimer == NULL) {
        return ExitCode_Init_SendTimer;
    }

    // Open a connection to the RTApp.
    sockFd = Application_Connect(rtAppComponentId);
    if (sockFd == -1) {
        Log_Debug("ERROR: Unable to create socket: %d (%s)\n", errno, strerror(errno));
        return ExitCode_Init_Connection;
    }

    // Set timeout, to handle case where real-time capable application does not respond.
    static const struct timeval recvTimeout = {.tv_sec = 5, .tv_usec = 0};
    int result = setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
    if (result == -1) {
        Log_Debug("ERROR: Unable to set socket timeout: %d (%s)\n", errno, strerror(errno));
        return ExitCode_Init_SetSockOpt;
    }

    // Register handler for incoming messages from real-time capable application.
    socketEventReg = EventLoop_RegisterIo(eventLoop, sockFd, EventLoop_Input, SocketEventHandler,
                                          /* context */ NULL);
    if (socketEventReg == NULL) {
        Log_Debug("ERROR: Unable to register socket event: %d (%s)\n", errno, strerror(errno));
        return ExitCode_Init_RegisterIo;
    }

    return ExitCode_Success;
}

/// <summary>
///     Closes a file descriptor and prints an error on failure.
/// </summary>
/// <param name="fd">File descriptor to close</param>
/// <param name="fdName">File descriptor name to use in error message</param>
static void CloseFdAndPrintError(int fd, const char *fdName)
{
    if (fd >= 0) {
        int result = close(fd);
        if (result != 0) {
            Log_Debug("ERROR: Could not close fd %s: %s (%d).\n", fdName, strerror(errno), errno);
        }
    }
}

/// <summary>
///     Clean up the resources previously allocated.
/// </summary>
static void CloseHandlers(void)
{
    DisposeEventLoopTimer(sendTimer);
    EventLoop_UnregisterIo(eventLoop, socketEventReg);
    EventLoop_Close(eventLoop);

    Log_Debug("Closing file descriptors.\n");
    CloseFdAndPrintError(sockFd, "Socket");
}


static ExitCode RunLoop(void)
{
    Log_Debug("Running main loop.\n");

    exitCode = InitHandlers();

    while (exitCode == ExitCode_Success) {
        EventLoop_Run_Result result = EventLoop_Run(eventLoop, -1, true);
        // Continue if interrupted by signal, e.g. due to breakpoint being set.
        if (result == EventLoop_Run_Failed && errno != EINTR) {
            exitCode = ExitCode_Main_EventLoopFail;
        }
    }

    CloseHandlers();

    if (exitCode == ExitCode_Main_EventLoopSimReboot) {
        Log_Debug("Simulating reboot...\n");
        static const struct timespec wait = {.tv_sec = 10, .tv_nsec = 0};
        if (nanosleep(&wait, NULL) == -1) {
            Log_Debug("WARNING: wait interrupted\n");
        }
        Log_Debug("Re-initialising\n");
    }

    return exitCode;
}

int main(void)
{
    Log_Debug("High-level intercore comms application\n");
    Log_Debug("Sends data to, and receives data from a real-time capable application.\n");

    InitSigterm();

    ExitCode code;

    while ((code = RunLoop()) == ExitCode_Main_EventLoopSimReboot);

    Log_Debug("Application exiting.\n");
    return exitCode;
}

/*
 *  kbhit implementation for Unix/POSIX systems
 *  Copyright (c) 2024, MafiaHub
 *  Licensed under MIT-style license
 */

#ifndef __KBHIT_H
#define __KBHIT_H

#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

inline int kbhit(void)
{
    struct timeval tv;
    fd_set fds;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
    return FD_ISSET(STDIN_FILENO, &fds);
}

#endif

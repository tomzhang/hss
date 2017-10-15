#ifndef _HSS_EXECUTOR_H_
#define _HSS_EXECUTOR_H_

#include "sstring.h"

struct slot;

void
reap_child_handler(int sig);

int
exec_remote_cmd(struct slot *pslot, char *cmd);

int
sync_exec_remote_cmd(struct slot *pslot, char *cmd, sstring *out, sstring *err);

int
exec_local_cmd(char *cmd);

int
exec_inner_cmd(char *line);

#endif //_HSS_EXECUTOR_H_
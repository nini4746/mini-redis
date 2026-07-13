#ifndef COMMAND_H
#define COMMAND_H

#include "conn.h"

/* Dispatch the command currently held in c->argv/c->argc.
 * Writes a RESP reply into the client's write buffer. */
void command_dispatch(client *c);

#endif

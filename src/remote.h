/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_REMOTE_H
#define SNAJPAGENT_REMOTE_H

/* Separate client-only entry point. No agent initialization is permitted here. */
int snag_remote_main(int argc, char **argv);

#endif

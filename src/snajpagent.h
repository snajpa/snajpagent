/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SNAJPAGENT_H
#define SNAJPAGENT_H

#ifndef SNAJPAGENT_NAME
#error "SNAJPAGENT_NAME must come from META through the build"
#endif
#ifndef SNAJPAGENT_VERSION
#error "SNAJPAGENT_VERSION must come from Git tags through the build"
#endif

#define SNAJPAGENT_IDENTITY SNAJPAGENT_NAME " " SNAJPAGENT_VERSION
#define SNAJPAGENT_MODEL "gpt-5.5-2026-04-23"
#define SNAJPAGENT_PROFILE_ID "bootstrap-openai-responses-gpt-5.5-2026-04-23-v1"
#define SNAJPAGENT_CAPABILITY_VERSION "2026-06-23.2-bootstrap"
#endif

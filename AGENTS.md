<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Project Instructions

KISS is the first engineering rule for this repository. Use the smallest design
that correctly solves the current problem. A larger design needs a concrete
reason from the code in front of you, not a guess about future needs.

When presenting options, make the simplest viable design the default
recommendation. Only offer a larger variant as an exception, and spell out the
current concrete reason it is needed.

Do not add a second knob, mode, abstraction, helper binary, or subsystem when
one clear behavior is enough.

When a provider lacks an optional native endpoint, keep the user-facing behavior
working through the existing provider path whenever practical.

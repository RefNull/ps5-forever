// ProsperoStore - Static miniz export configuration.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define MINIZ_EXPORT
/* The worker reads archives only through mz_zip_reader_init callbacks; drop
 * miniz's FILE* paths (fseeko/ftello are not declared under strict C11 on
 * x86_64 glibc, and the code is unused attack surface). */
#define MINIZ_NO_STDIO

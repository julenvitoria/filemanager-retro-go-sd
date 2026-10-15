#pragma once
/* Only the inflate part of miniz (tinfl) is built in this project. */
#ifndef MINIZ_NO_MALLOC
#define MINIZ_NO_MALLOC
#endif
#ifndef MINIZ_NO_STDIO
#define MINIZ_NO_STDIO
#endif
#ifndef MINIZ_NO_TIME
#define MINIZ_NO_TIME
#endif
#include "miniz_common.h"
#include "miniz_tinfl.h"

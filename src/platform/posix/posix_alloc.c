//
// Copyright 2016 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#ifdef PLATFORM_POSIX_ALLOC

#include <stdlib.h>

// POSIX memory allocation.  This is pretty much standard C.
void *
nni_alloc(size_t sz)
{
	return (calloc(1, sz));
}

void
nni_free(void *ptr, size_t size)
{
	NNI_ARG_UNUSED(size);
	free(ptr);
}

#else

// Suppress empty symbols warnings in ranlib.
int nni_posix_alloc_not_used = 0;

#endif

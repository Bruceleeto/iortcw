/*
 * new and delete over malloc and free, with no exceptions: the C++
 * runtime's would throw bad_alloc, and bring in its exception handling and
 * demangler for it. With -fno-exceptions there is nothing to catch it.
 */
#ifdef SPLINES_OWN_NEW
#include <stdlib.h>
#include <new>

void *operator new( size_t size ) {
	return malloc( size ? size : 1 );
}

void *operator new[]( size_t size ) {
	return malloc( size ? size : 1 );
}

void operator delete( void *p ) noexcept {
	free( p );
}

void operator delete[]( void *p ) noexcept {
	free( p );
}

void operator delete( void *p, size_t ) noexcept {
	free( p );
}

void operator delete[]( void *p, size_t ) noexcept {
	free( p );
}
#endif

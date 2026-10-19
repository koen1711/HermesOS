#ifndef OS_ERRNO_H
#define OS_ERRNO_H

#define EPERM            1  // operation not permitted
#define ENOENT           2  // no such file or directory
#define ESRCH            3  // no such process
#define EINTR            4  // interrupted system call
#define EIO              5  // I/O error
#define ENXIO            6  // no such device or address
#define E2BIG            7  // argument list too long
#define ENOEXEC          8  // exec format error
#define EBADF            9  // bad file descriptor
#define ECHILD          10  // no child processes
#define EAGAIN          11  // try again
#define EWOULDBLOCK     EAGAIN
#define ENOMEM          12  // out of memory
#define EACCES          13  // permission denied
#define EFAULT          14  // bad address
#define ENOTBLK         15  // block device required
#define EBUSY           16  // device or resource busy
#define EEXIST          17  // file exists
#define EXDEV           18  // cross-device link
#define ENODEV          19  // no such device
#define ENOTDIR         20  // not a directory
#define EISDIR          21  // is a directory
#define EINVAL          22  // invalid argument
#define ENFILE          23  // file table overflow
#define EMFILE          24  // too many open files
#define ENOTTY          25  // not a typewriter
#define ETXTBSY         26  // text file busy
#define EFBIG           27  // file too large
#define ENOSPC          28  // no space left on device
#define ESPIPE          29  // illegal seek
#define EROFS           30  // read-only file system
#define EMLINK          31  // too many links
#define EPIPE           32  // broken pipe
#define EDOM            33  // math argument out of domain
#define ERANGE          34  // math result not representable
#define EDEADLK         35  // resource deadlock would occur
#define ENAMETOOLONG    36  // file name too long
#define ENOLCK          37  // no record locks available
#define ENOSYS          38  // invalid system call number
#define ENOTEMPTY       39  // directory not empty
#define ELOOP           40  // too many symbolic links encountered
#define ENOMSG          42  // no message of desired type
#define ENODATA         61  // no data available
#define ETIME           62  // timer expired
#define EPROTO          71  // protocol error
#define EBADMSG         74  // not a data message
#define EOVERFLOW       75  // value too large for defined data type
#define EILSEQ          84  // illegal byte sequence

// networking
#define ENOTSOCK        88  // socket operation on non-socket
#define EDESTADDRREQ    89  // destination address required
#define EMSGSIZE        90  // message too long
#define EPROTOTYPE      91  // protocol wrong type for socket
#define ENOPROTOOPT     92  // protocol not available
#define EPROTONOSUPPORT 93  // protocol not supported
#define ESOCKTNOSUPPORT 94  // socket type not supported
#define EOPNOTSUPP      95  // operation not supported on transport endpoint
#define ENOTSUP         EOPNOTSUPP
#define EPFNOSUPPORT    96  // protocol family not supported
#define EAFNOSUPPORT    97  // address family not supported by protocol
#define EADDRINUSE      98  // address already in use
#define EADDRNOTAVAIL   99  // cannot assign requested address
#define ENETDOWN       100  // network is down
#define ENETUNREACH    101  // network is unreachable
#define ENETRESET      102  // network dropped connection because of reset
#define ECONNABORTED   103  // software caused connection abort
#define ECONNRESET     104  // connection reset by peer
#define ENOBUFS        105  // no buffer space available
#define EISCONN        106  // transport endpoint is already connected
#define ENOTCONN       107  // transport endpoint is not connected
#define ESHUTDOWN      108  // cannot send after transport endpoint shutdown
#define ETOOMANYREFS   109  // too many references: cannot splice
#define ETIMEDOUT      110  // connection timed out
#define ECONNREFUSED   111  // connection refused
#define EHOSTDOWN      112  // host is down
#define EHOSTUNREACH   113  // no route to host
#define EALREADY       114  // operation already in progress
#define EINPROGRESS    115  // operation now in progress
#define ECANCELED      125  // operation canceled

#endif //OS_ERRNO_H

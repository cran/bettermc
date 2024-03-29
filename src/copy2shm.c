#define _GNU_SOURCE
#include "bettermc.h"

SEXP is_altrep(SEXP x) {
  return ScalarLogical(ALTREP(x));
}

SEXP is_allocated(SEXP x) {
  return ScalarLogical(DATAPTR_OR_NULL(x) != NULL);
}

#ifndef _WIN32

#include <R_ext/Rallocators.h>
#include <R_ext/Altrep.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>

#include "vg/memcheck.h"

// https://github.com/wch/r-source/blob/tags/R-3-6-3/src/include/Defn.h#L409-L422
typedef struct {
  union {
    SEXP		backpointer;
    double		align;
  } u;
} VECREC;

/* Vector Heap Macros */
#define BYTE2VEC(n)	(((n)>0)?(((n)-1)/sizeof(VECREC)+1):0)
#define INT2VEC(n)	(((n)>0)?(((n)*sizeof(int)-1)/sizeof(VECREC)+1):0)
#define FLOAT2VEC(n)	(((n)>0)?(((n)*sizeof(double)-1)/sizeof(VECREC)+1):0)
#define COMPLEX2VEC(n)	(((n)>0)?(((n)*sizeof(Rcomplex)-1)/sizeof(VECREC)+1):0)

sigjmp_buf senv;
void handle_bus_error(int sig) {
  siglongjmp(senv, 1);
}

SEXP copy2shm(SEXP x, SEXP n, SEXP overwrite, SEXP huge_threshold) {
  const char *name = CHAR(STRING_ELT(n, 0));

  int oflag = O_CREAT | O_RDWR;
  if (asLogical(overwrite)) {
    oflag |= O_TRUNC;
  } else {
    oflag |= O_EXCL;
  }

  int fd = shm_open(name, oflag, S_IRUSR | S_IWUSR);
  if (fd == -1) {
    char buf[100];
    snprintf(buf, 100, "'shm_open' failed with '%s'", strerror(errno));
    return mkString(buf);
  }

  SEXP off = allocVector(RAWSXP, 1);
  size_t offset = sizeof(R_allocator_t) + ((char *) DATAPTR(off) - (char *) off);
  size_t data_size;

  switch(TYPEOF(x)) {
  case LGLSXP:
  case INTSXP:
    data_size = INT2VEC(XLENGTH(x)) * sizeof(VECREC);
    break;
  case REALSXP:
    data_size = FLOAT2VEC(XLENGTH(x)) * sizeof(VECREC);
    break;
  case CPLXSXP:
    data_size = COMPLEX2VEC(XLENGTH(x)) * sizeof(VECREC);
    break;
  case RAWSXP:
    data_size = BYTE2VEC(XLENGTH(x)) * sizeof(VECREC);
    break;
  default:
    error("unsupported SEXP type for 'x': %s", type2char(TYPEOF(x)));
  }

  /* The main reason for using ftruncate() rather than posix_fallocate() here is
   * that with the latter madvise(..., MADV_HUGEPAGE) is not honored.
   *
   * The downside of using ftruncate() is that if there is actually not enough
   * shared memory left, memcpy() results in a SIGBUS, which we have to handle.
   *
   * On the contrary: "After a successful call to posix_fallocate(), subsequent
   * writes to bytes in the specified range are guaranteed not to fail because
   * of lack of disk space."
   */
  if (ftruncate(fd, data_size + offset) == -1) {
    close(fd);
    shm_unlink(name);

    char buf[100];
    snprintf(buf, 100, "'ftruncate' failed with '%s'", strerror(errno));
    return mkString(buf);
  }

  char *sptr = mmap(NULL, data_size + offset,
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (sptr == MAP_FAILED) {
    close(fd);
    shm_unlink(name);

    char buf[100];
    snprintf(buf, 100, "'mmap' failed with '%s'", strerror(errno));
    return mkString(buf);
  }

  close(fd);

#ifdef MADV_HUGEPAGE
  if (data_size + offset >= asReal(huge_threshold)) {
    if (madvise(sptr, data_size + offset, MADV_HUGEPAGE) == -1) {
      munmap(sptr, data_size + offset);
      shm_unlink(name);

      char buf[100];
      snprintf(buf, 100, "'madvise' failed with '%s'", strerror(errno));
      return mkString(buf);
    }
  }
#endif

  /* Here we do the actual memcpy() and handle a potential SIGBUS.
   *
   * The signal handler must do a longjump rather than simply set some global
   * status variable and return, because then memcpy() would continue right
   * where it left of, which would immediately result in the next SIGBUS.
   *
   */
  struct sigaction bussa;
  struct sigaction oldsa;
  bussa.sa_handler = handle_bus_error;
  bussa.sa_flags = 0;
  sigemptyset(&bussa.sa_mask);

  sigset_t oldset;
  sigset_t busset;
  sigemptyset(&busset);
  sigaddset(&busset, SIGBUS);


  if (sigsetjmp(senv, 0) == 0) {
    sigaction(SIGBUS, &bussa, &oldsa);
    sigprocmask(SIG_UNBLOCK, &busset, &oldset);

    memcpy(sptr + offset, DATAPTR(x), data_size);
  } else {
    // there was a SIGBUS
    sigprocmask(SIG_SETMASK, &oldset, NULL);
    sigaction(SIGBUS, &oldsa, NULL);

    munmap(sptr, data_size + offset);
    shm_unlink(name);
    return mkString("'memcpy' resulted in a SIGBUS (no shared memory left)");
  }

  sigprocmask(SIG_SETMASK, &oldset, NULL);
  sigaction(SIGBUS, &oldsa, NULL);



  munmap(sptr, data_size + offset);

  SEXP ret = PROTECT(allocVector(VECSXP, 5));
  SET_VECTOR_ELT(ret, 0, PROTECT(duplicate(n)));
  SET_VECTOR_ELT(ret, 1, PROTECT(ScalarInteger(TYPEOF(x))));
  SET_VECTOR_ELT(ret, 2, PROTECT(ScalarReal(XLENGTH(x))));
  SET_VECTOR_ELT(ret, 3, PROTECT(ScalarReal(data_size + offset)));
  SET_VECTOR_ELT(ret, 4, PROTECT(shallow_duplicate(ATTRIB(x))));

  UNPROTECT(6);
  return ret;
}

typedef struct allocator_data {
  void *ptr;
  size_t size;
} allocator_data;

void* shm_alloc(R_allocator_t *allocator, size_t size) {
  allocator_data *data = allocator->data;
  if (size != data->size) {  // this should never happen (did X2VEC change in R?)
    munmap(data->ptr, data->size);

    size_t expected_size = data->size;
    free(data);

    error("'shm_alloc' was asked for %zu bytes but expected %zu bytes.",
          size, expected_size);
  }
  return data->ptr;
}

void shm_free(R_allocator_t *allocator, void *addr) {
  allocator_data *data = allocator->data;
  if (addr != data->ptr) {  // this should never happen (bug in R?)
    // something is very fishy here; don't even try to cleanup
    error("'addr' not equal to 'data->ptr' in 'shm_free'");
  }

  munmap(data->ptr, data->size);
  free(data);
}

SEXP allocate_from_shm(SEXP name, SEXP type, SEXP length, SEXP size,
                       SEXP attributes, SEXP copy) {
  int fd = shm_open(CHAR(STRING_ELT(name, 0)), O_RDWR, 0);
  shm_unlink(CHAR(STRING_ELT(name, 0)));
  if (fd == -1) {
    error("'shm_open' failed with '%s'\n", strerror(errno));
  }

  struct stat sb;
  if (fstat(fd, &sb) == -1) {
    close(fd);
    error("'fstat' failed with '%s'\n", strerror(errno));
  }

#ifdef CHECK_EXACT_SHM_OBJ_SIZE
  // used on Linux
  if (sb.st_size != asReal(size)) {
    close(fd);
    error("file backing shm object is of wrong size; expected: %.0f bytes, actual: %ld bytes",
          asReal(size), sb.st_size);
  }
#else
  // used on macOS, which reports the size in multiples of page size
  long pagesize = sysconf(_SC_PAGESIZE);
  size_t pages = (size_t) asReal(size) / pagesize + 1;

  if (sb.st_size != pages * pagesize) {
    close(fd);
    error("file backing shm object is of wrong size; expected: %ld bytes, actual: %ld bytes",
          pages * pagesize, sb.st_size);
  }
#endif

  void *sptr;
  if (asLogical(copy)) {
    // here we use MAP_SHARED because we only copy from the mmaped region to the
    // regularly allocated R vector and macOS does not support MAP_PRIVATE
    sptr = mmap(NULL, asReal(size),
                PROT_READ, MAP_SHARED, fd, 0);
  } else {
    // MAP_PRIVATE is crucial here; using MAP_SHARED would make unit test
    // "changes to vectors allocate(d)_from_shm are private" fail;
    // the caller ensures that we do not end up in this branch on macOS
    sptr = mmap(NULL, asReal(size),
                PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  }

  close(fd);

  if (sptr == MAP_FAILED) {
    error("'mmap' failed with '%s'\n", strerror(errno));
  }

  allocator_data* data = malloc(sizeof(allocator_data));
  if (data == NULL) {
    error("'malloc' failed to allocate %zu bytes", sizeof(allocator_data));
  }

  data->ptr = sptr;
  data->size = asReal(size);

  R_allocator_t allocator;
  allocator.mem_alloc = &shm_alloc;
  allocator.mem_free = &shm_free;
  allocator.res = NULL;
  allocator.data = data;


  size_t expected_size;
  size_t dataptr_size;
  switch(asInteger(type)) {
  case LGLSXP:
  case INTSXP:
    expected_size = INT2VEC((R_xlen_t) asReal(length)) * sizeof(VECREC);
    dataptr_size = asReal(length) * sizeof(int);
    break;
  case REALSXP:
    expected_size = FLOAT2VEC((R_xlen_t) asReal(length)) * sizeof(VECREC);
    dataptr_size = asReal(length) * sizeof(double);
    break;
  case CPLXSXP:
    expected_size = COMPLEX2VEC((R_xlen_t) asReal(length)) * sizeof(VECREC);
    dataptr_size = asReal(length) * sizeof(Rcomplex);
    break;
  case RAWSXP:
    expected_size = BYTE2VEC((R_xlen_t) asReal(length)) * sizeof(VECREC);
    dataptr_size = asReal(length);
    break;
  default:
    shm_free(&allocator, sptr);
    error("unsupported SEXP type: %s", type2char(asInteger(type)));
  }

  SEXP off = allocVector(RAWSXP, 1);
  size_t offset = sizeof(R_allocator_t) + ((char *) DATAPTR(off) - (char *) off);
  if (data->size - offset != expected_size) {
    shm_free(&allocator, sptr);
    error("'alloc_from_shm' expected a shared memory object with %zu bytes but it has %zu bytes.",
          expected_size + offset, (size_t) asReal(size));
  }


  SEXP ret;
  if (!asLogical(copy) && asReal(length) >= 2) {
    ret = PROTECT(allocVector3(asInteger(type), asReal(length), &allocator));
    VALGRIND_MAKE_MEM_DEFINED(DATAPTR(ret), dataptr_size);
  } else {
    ret = PROTECT(allocVector(asInteger(type), asReal(length)));

    memcpy(DATAPTR(ret), (char *) sptr + offset, dataptr_size);

    shm_free(&allocator, sptr);
  }

  SEXP a = PROTECT(shallow_duplicate(attributes));
  SET_ATTRIB(ret, a);
  SEXP cls = getAttrib(ret, R_ClassSymbol);
  if (! isNull(cls)) classgets(ret, cls);

  UNPROTECT(2);
  return ret;
}

SEXP unlink_all_shm(SEXP prefix, SEXP start) {
  const char *pre = CHAR(STRING_ELT(prefix, 0));
  int pre_len = strlen(pre);

  int total_len = pre_len + 11;
  char buf[total_len];

  int i = asInteger(start);
  while (TRUE) {
    snprintf(buf, total_len, "%s%d", pre, i);
    if (shm_unlink(buf) == -1) {
      if (errno == ENOENT) {
        break;
      } else {
        error("'shm_unlink' failed with '%s'\n", strerror(errno));
      }
    }
    i++;
  }

  return R_NilValue;
}

#else

SEXP copy2shm(SEXP x, SEXP n, SEXP overwrite, SEXP huge_threshold) {
  error("Not supported on Windows.");
}

SEXP allocate_from_shm(SEXP name, SEXP type, SEXP length, SEXP size,
                       SEXP attributes, SEXP copy) {
  error("Not supported on Windows.");
}

SEXP unlink_all_shm(SEXP prefix, SEXP start) {
  error("Not supported on Windows.");
}

#endif

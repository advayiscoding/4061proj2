#define _GNU_SOURCE

#include <Python.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hadamard.h"

/*
 * Function executed in each child process to compute its portion of the hadamard product
 * Declared here so it can be referenced in hadamard_shmem. Fill in the definition at
 * the end of this file.
 *
 * 'a': Pointer to shared memory segment storing first matrix operand
 * 'b': Pointer to shared memory segment storing second matrix operand
 * 'c': Pointer to shared memory segment storing result matrix
 * 'start': Index (in flattened representation) of first element of matrix portion
 * 'end': Index (in flattened representation) one PAST the last element of the
 *        matrix portion (i.e., the range is [start, end) )
 * 'mapped_size': Size (in bytes) of each shared memory segment
 *
 * Returns 0 on success or -1 on failure
 */
static int worker(int *a, int *b, int *c, unsigned start, unsigned end, unsigned mapped_size);

/*
 * Set a Python OSError from the current errno, but only if no Python error
 * has been set yet (so the FIRST failure is the one reported).
 */
static void set_error_from_errno(void)
{
    if (!PyErr_Occurred())
    {
        PyErr_SetFromErrno(PyExc_OSError);
    }
}

PyObject *hadamard_shmem(PyObject *self, PyObject *args)
{
    // ---- Step 1: read arguments passed from Python ----
    const char *mat_a_file;
    const char *mat_b_file;
    unsigned n_procs;
    if (!PyArg_ParseTuple(args, "ssI", &mat_a_file, &mat_b_file, &n_procs))
    {
        // ParseTuple sets up a Python exception on our behalf
        return NULL;
    }

    // ---- Step 2: read A and B straight into SHARED memory (sh_mem = 1) ----
    unsigned a_rows;
    unsigned a_cols;
    int *mat_a_data = read_matrix_from_file(mat_a_file, &a_rows, &a_cols, 1);
    if (mat_a_data == NULL)
    {
        PyErr_SetString(PyExc_OSError, "Failed to read matrix data from file");
        return NULL;
    }
    unsigned b_rows;
    unsigned b_cols;
    int *mat_b_data = read_matrix_from_file(mat_b_file, &b_rows, &b_cols, 1);
    if (mat_b_data == NULL)
    {
        PyErr_SetString(PyExc_OSError, "Failed to read matrix data from file");
        unsigned a_size = a_rows * a_cols * sizeof(int);
        if (munmap(mat_a_data, a_size) == -1)
        {
            perror("munmap");
        }
        return NULL;
    }
    assert(a_rows == b_rows && a_cols == b_cols);

    unsigned n_elems = a_rows * a_cols;
    // Same size as the segments read_matrix_from_file created for A and B
    unsigned mapped_size = n_elems * sizeof(int);

    // ---- Step 3: shared segment for the result matrix C ----
    // Same call read_matrix_from_file uses. MAP_SHARED is what makes the
    // children's writes to C visible to the parent after fork().
    int *mat_c_data =
        mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mat_c_data == MAP_FAILED)
    { // mmap signals failure with MAP_FAILED, not NULL
        set_error_from_errno();
        if (munmap(mat_a_data, mapped_size) == -1)
        {
            perror("munmap");
        }
        if (munmap(mat_b_data, mapped_size) == -1)
        {
            perror("munmap");
        }
        return NULL;
    }

    // ---- Step 4: fork one worker per chunk ----
    // Ceiling division so every element is covered: chunk = ceil(n_elems / n_procs)
    unsigned chunk = (n_elems + n_procs - 1) / n_procs;
    unsigned n_launched = 0;
    for (unsigned i = 0; i < n_procs; i++)
    {
        unsigned start = min(i * chunk, n_elems);
        unsigned end = min(start + chunk, n_elems);

        pid_t pid = fork();
        if (pid == -1)
        {
            set_error_from_errno();
            break; // still need to wait for the children already running
        }
        else if (pid == 0)
        {
            // CHILD: compute its slice directly into shared C, then leave.
            // MUST exit: returning would drop this child back into Python.
            int rc = worker(mat_a_data, mat_b_data, mat_c_data, start, end, mapped_size);
            exit(rc == 0 ? 0 : 1);
        }
        n_launched++;
    }

    // ---- Step 5: wait for every child (this is the ONLY coordination) ----
    // After wait() returns for all children, every write into C is finished.
    for (unsigned i = 0; i < n_launched; i++)
    {
        int status;
        if (wait(&status) == -1)
        {
            set_error_from_errno();
        }
        else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            if (!PyErr_Occurred())
            {
                PyErr_SetString(PyExc_OSError, "A worker process failed");
            }
        }
    }

    // ---- Step 6: print (only if everything worked), then unmap all segments ----
    if (!PyErr_Occurred())
    {
        print_matrix(mat_c_data, a_rows, a_cols);
    }
    if (munmap(mat_a_data, mapped_size) == -1)
    {
        set_error_from_errno();
    }
    if (munmap(mat_b_data, mapped_size) == -1)
    {
        set_error_from_errno();
    }
    if (munmap(mat_c_data, mapped_size) == -1)
    {
        set_error_from_errno();
    }

    if (PyErr_Occurred())
    {
        return NULL;
    }
    Py_RETURN_NONE;
}

// Called once per child process
static int worker(int *a, int *b, int *c, unsigned start, unsigned end, unsigned mapped_size)
{
    // Each child touches ONLY [start, end), so no two children ever write the
    // same location -- no locks needed.
    for (unsigned i = start; i < end; i++)
    {
        c[i] = a[i] * b[i];
    }

    // Cleanup: detach this child's mappings. This only removes the child's
    // view; the parent's mappings (and the data in C) are unaffected.
    int ret = 0;
    if (munmap(a, mapped_size) == -1)
    {
        perror("munmap");
        ret = -1;
    }
    if (munmap(b, mapped_size) == -1)
    {
        perror("munmap");
        ret = -1;
    }
    if (munmap(c, mapped_size) == -1)
    {
        perror("munmap");
        ret = -1;
    }
    return ret;
}

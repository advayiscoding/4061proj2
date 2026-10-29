#define _GNU_SOURCE

#include <Python.h>
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hadamard.h"

/*
 * Represents arguments for each thread
 * 'a': Pointer to first matrix operand
 * 'b': Pointer to second matrix operand
 * 'c': Pointer to result matrix
 * 'start': Index (in flattened representation) of first element of matrix portion
 * 'end': Index (in flattened representation) of second element of matrix portion
 */
struct thread_args
{
    int *a;
    int *b;
    int *c;
    unsigned start;
    unsigned end;
};

/*
 * Function executed in each thread to compute its portion of the hadamard product
 * 'data': Pointer to struct thread_args instance
 * Declared here so this function can be referenced in hadamard_threads().
 * Complete the definition at the bottom of this file.
 * Returns NULL
 */
static void *worker(void *data);

PyObject *hadamard_threads(PyObject *self, PyObject *args)
{
    /*
     * TODO Complete this function
     * Read arguments passed in by Python
     * Read the two input matrices from their files
     * Set up a result matrix on the heap
     * Launch 'n_workers' threads, each assigned a roughly equal portion of the
     * matrices to work on.
     * Wait for all threads to complete, then pass the result matrix to the
     * print_matrix() function.
     * Don't forget error handling and cleanup!
     */
    const char *file_1; // first matrix file name
    const char *file_2; // second matrix file name
    unsigned n_workers; // number of workers

    if (!PyArg_ParseTuple(args, "ssI", &file_1, &file_2, &n_workers)) // translates the python vals into c and ssI is the order string, string, unsigned int
    {
        return NULL;
    }

    if (n_workers <= 0)
    {
        PyErr_SetString(PyExc_ValueError, "Number of workers must be greater than 0");
        return NULL;
    }

    unsigned a_rows, a_cols, b_rows, b_cols;

    int *a = read_matrix_from_file(file_1, &a_rows, &a_cols, 0);
    int *b = read_matrix_from_file(file_2, &b_rows, &b_cols, 0);

    if (a == NULL || b == NULL)
    {
        free(a);
        free(b);
        return NULL;
    }

    assert(a_rows == b_rows && a_cols == b_cols);

    size_t total = a_rows * a_cols;

    int *c = malloc(total * sizeof(int));

    if (c == NULL)
    {
        free(a);
        free(b);
        return PyErr_NoMemory();
    }

    pthread_t *threads = calloc(n_workers, sizeof(*threads));

    struct thread_args *worker_args =
        calloc(n_workers, sizeof(*worker_args));

    if (threads == NULL || worker_args == NULL)
    {
        free(worker_args);
        free(threads);
        free(c);
        free(b);
        free(a);
        return PyErr_NoMemory();
    }

    size_t base = total / n_workers; // divie the work
    size_t remainder = total % n_workers;

    unsigned launched = 0;
    int create_error = 0;
    int join_error = 0;

    // Give each worker its instruction and start its thread
    for (unsigned t = 0; t < n_workers; t++)
    {
        size_t start =
            t * base + (t < remainder ? t : remainder);
        size_t end =
            start + base + (t < remainder);

        worker_args[t] = (struct thread_args){
            .a = a,
            .b = b,
            .c = c,
            .start = (unsigned)start,
            .end = (unsigned)end};

        create_error = pthread_create(
            &threads[t],
            NULL,
            worker,
            &worker_args[t]);

        if (create_error != 0)
        {
            break;
        }

        launched++;
    }

    for (unsigned t = 0; t < launched; t++)
    {
        int err = pthread_join(threads[t], NULL);

        if (err != 0 && join_error == 0)
        {
            join_error = err;
        }
    }

    if (join_error != 0)
    {
        PyErr_Format(PyExc_RuntimeError,
                     "pthread_join: %s", strerror(join_error));
        return NULL;
    }

    if (create_error == 0)
    {
        print_matrix(c, a_rows, a_cols);
    }

    free(worker_args);
    free(threads);
    free(c);
    free(b);
    free(a);

    if (create_error != 0)
    {
        PyErr_Format(PyExc_RuntimeError,
                     "pthread_create: %s", strerror(create_error));
        return NULL;
    }

    Py_RETURN_NONE;
}

void *worker(void *p)
{
    /*
     * TODO Complete this function
     * Multiply each pair of elements and store the result, for the assigned
     * portion of the input matrices.
     */
    struct thread_args *args = p;

    for (unsigned i = args->start; i < args->end; i++)
    {
        args->c[i] = args->a[i] * args->b[i];
    }

    return NULL;
}

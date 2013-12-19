#include <R.h>
#define USE_RINTERNALS
#include <Rinternals.h>
// #include <signal.h> // the debugging machinery + breakpoint aidee

// for tolerance
extern SEXP fastradixint(SEXP vec, SEXP return_index, SEXP decreasing);

// adapted from Michael Herf's code - http://stereopsis.com/radix.html
// TO IMPLEMENT (probably) - R's long vector support
// also allows 'return_index' argument to return sort 'order' or 'value' (but not both currently)
// also allows ordering/sorting with 'tolerance' (another pass through length of input vector will happen + multiple integer radix sort calls). The performance will depend on the number of groups that have to be sorted because under given tolerance they become identical.
// Added 14th Dec: also now sorts/orders in descending order (argument decreasing=TRUE)
unsigned long flip_double(unsigned long f) {
    unsigned long mask = -(long)(f >> 63) | 0x8000000000000000;
    return f ^ mask;
}

void flip_double_ref(unsigned long *f) {
    unsigned long mask = -(long)(*f >> 63) | 0x8000000000000000;
    *f ^= mask;
}

unsigned long invert_flip_double(unsigned long f) {
    unsigned long mask = ((f >> 63) - 1) | 0x8000000000000000;
    return f ^ mask;
}

void flip_double_decr(unsigned long *f) {
    *f ^= 0x8000000000000000;
}


// utils for accessing 11-bit quantities
#define _0(x) (x & 0x7FF)
#define _1(x) (x >> 11 & 0x7FF)
#define _2(x) (x >> 22 & 0x7FF)
#define _3(x) (x >> 33 & 0x7FF)
#define _4(x) (x >> 44 & 0x7FF)
#define _5(x) (x >> 55)

// x should be of type numeric
SEXP fastradixdouble(SEXP x, SEXP tol, SEXP return_index, SEXP decreasing) {
    int i;
    unsigned long pos, fi, si, n;
    unsigned long sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0, sum4 = 0, sum5 = 0, tsum;    
    SEXP xtmp, order, ordertmp;
    
    n = length(x);
    if (!isReal(x) || n <= 0) error("List argument to 'fradix' must be non-empty and of type 'numeric'");
    if (TYPEOF(return_index) != LGLSXP || length(return_index) != 1) error("Argument 'return_index' to 'fradix' must be logical TRUE/FALSE");
    if (TYPEOF(decreasing) != LGLSXP || length(decreasing) != 1 || LOGICAL(decreasing)[0] == NA_LOGICAL) error("Argument 'decreasing' to 'fradix' must be logical TRUE/FALSE");
    if (TYPEOF(tol) != REALSXP) error("Argument 'tol' to 'fradix' must be a numeric vector of length 1");


    xtmp  = PROTECT(allocVector(REALSXP, n));
    ordertmp = PROTECT(allocVector(INTSXP, n));
    order = PROTECT(allocVector(INTSXP, n));
    
    unsigned long *array = (unsigned long*)REAL(x);
    unsigned long *sort = (unsigned long*)REAL(xtmp);
            
    // 6 histograms on the stack:
    const unsigned long stack_hist = 2048;
    unsigned long b0[stack_hist * 6];
    unsigned long *b1 = b0 + stack_hist;
    unsigned long *b2 = b1 + stack_hist;
    unsigned long *b3 = b2 + stack_hist;
    unsigned long *b4 = b3 + stack_hist;
    unsigned long *b5 = b4 + stack_hist;

    // definitely faster on big data than a for-loop
    memset(b0, 0, stack_hist*6*sizeof(unsigned long));

    // Step 1:  parallel histogramming pass
    for (i=0;i<n;i++) {
        // NA in unsigned long is 7ff00000000007a2 and NaN is 7ff8000000000000
        // flip the sign bit to get fff00000000007a2 and NaN is fff8000000000000
        // this'll result in the right order (NaN first, then NA)
        // flip NaN/NA sign bit so that they get sorted in the front (special for data.table)
        if (ISNAN(REAL(x)[i])) flip_double_ref(&array[i]);
        if (LOGICAL(decreasing)[0]) flip_double_decr(&array[i]);
        fi = flip_double((unsigned long)array[i]);
        b0[_0(fi)]++;
        b1[_1(fi)]++;
        b2[_2(fi)]++;
        b3[_3(fi)]++;
        b4[_4(fi)]++;
        b5[_5(fi)]++;
    }
    
    // Step 2:  Sum the histograms -- each histogram entry records the number of values preceding itself.
    for (i=0;i<stack_hist;i++) {

        tsum = b0[i] + sum0;
        b0[i] = sum0 - 1;
        sum0 = tsum;

        tsum = b1[i] + sum1;
        b1[i] = sum1 - 1;
        sum1 = tsum;

        tsum = b2[i] + sum2;
        b2[i] = sum2 - 1;
        sum2 = tsum;

        tsum = b3[i] + sum3;
        b3[i] = sum3 - 1;
        sum3 = tsum;

        tsum = b4[i] + sum4;
        b4[i] = sum4 - 1;
        sum4 = tsum;

        tsum = b5[i] + sum5;
        b5[i] = sum5 - 1;
        sum5 = tsum;
    }

    for (i=0;i<n;i++) {
        fi = array[i];
        flip_double_ref(&fi);
        pos = _0(fi);
        sort[++b0[pos]] = fi;
        INTEGER(ordertmp)[b0[pos]] = i;
    }

    for (i=0;i<n;i++) {
        si = sort[i];
        pos = _1(si);
        array[++b1[pos]] = si;
        INTEGER(order)[b1[pos]] = INTEGER(ordertmp)[i];
    }

    for (i=0;i<n;i++) {
        fi = array[i];
        pos = _2(fi);
        sort[++b2[pos]] = fi;
        INTEGER(ordertmp)[b2[pos]] = INTEGER(order)[i];
    }

    for (i=0;i<n;i++) {
        si = sort[i];
        pos = _3(si);
        array[++b3[pos]] = si;
        INTEGER(order)[b3[pos]] = INTEGER(ordertmp)[i];
    }

    for (i=0;i<n;i++) {
        fi = array[i];
        pos = _4(fi);
        sort[++b4[pos]] = fi;
        INTEGER(ordertmp)[b4[pos]] = INTEGER(order)[i];
    }

    for (i=0;i<n;i++) {
        si = sort[i];
        pos = _5(si);
        array[++b5[pos]] = invert_flip_double(si);
        if (LOGICAL(decreasing)[0]) flip_double_decr(&array[b5[pos]]);
        INTEGER(order)[b5[pos]] = INTEGER(ordertmp)[i]+1;
    }

    // NOTE: that the result won't be 'exactly' identical to ordernumtol if there are too many values that are 'very close' to each other.
    // However, I believe this is the correct version in those cases. Why? if you've 3 numbers that are sorted with no tolerance, and now under tolerance
    // these numbers are equal, then all 3 indices *must* be sorted. That is ther right order. But in some close cases, 'ordernumtol' doesn't do this.
    // To test, do: x <- rnorm(1e6); and run fastradixdouble and ordernumtol with same tolerance and compare results.

    // check for tolerance and reorder wherever necessary
    if (length(tol) > 0) {
        i=1;
        int j, start=0, end=0;
        SEXP st,dst,rt,sq,ridx,dec;
        PROTECT(ridx = allocVector(LGLSXP, 1));
        PROTECT(dec = allocVector(LGLSXP, 1));
        LOGICAL(ridx)[0] = TRUE;
        LOGICAL(dec)[0] = FALSE;
        while(i<n) {
            if (!LOGICAL(decreasing)[0]) {
                if (!R_FINITE(REAL(x)[i]) || !R_FINITE(REAL(x)[i-1])) { i++; continue; }
                // hack to skip checking Inf=Inf, -Inf=-Inf, NA=NA and NaN=NaN... using unsigned int
                if (REAL(x)[i]-REAL(x)[i-1] > REAL(tol)[0] || array[i] == array[i-1]) { i++; continue; }
                start = i-1;
                i++;
                while(i < n && REAL(x)[i] - REAL(x)[i-1] < REAL(tol)[0]) { i++; }
                end = i-1;
                i++;
                if (end-start+1 == 1) continue;
                PROTECT(st = allocVector(INTSXP, end-start+1));
                PROTECT(sq = allocVector(REALSXP, end-start+1));
                // To investigate: probably a simple bubble sort or shell sort may be quicker on groups with < 10 items than a 3-pass radix?
                // Can't rely on R's base radix order because even if you've two items in group and one of them is 3 and the other is 1e6, then it won't work!
                // Just doing this gives 4x speed-up under small group sizes. (from 37 to 6-9 seconds)
                if (end-start+1 == 2) {
                    // avoid radix sort on 2 items
                    if (INTEGER(order)[start] > INTEGER(order)[end]) {
                        // then just swap
                        INTEGER(st)[0] = INTEGER(order)[start];
                        INTEGER(order)[start] = INTEGER(order)[end];
                        INTEGER(order)[end] = INTEGER(st)[0];
                
                        REAL(sq)[0] = REAL(x)[start];
                        REAL(x)[start] = REAL(x)[end];
                        REAL(x)[end] = REAL(sq)[0];
                    }
                    UNPROTECT(2); // st, sq
                    continue;
                }
                for (j=0; j<end-start+1; j++) {
                    INTEGER(st)[j] = INTEGER(order)[j+start];
                    REAL(sq)[j] = REAL(x)[j+start];
                }
                PROTECT(dst = duplicate(st));
                PROTECT(rt = fastradixint(dst, ridx, decreasing));
                for (j=0; j<end-start+1; j++) {
                    INTEGER(order)[j+start] = INTEGER(st)[INTEGER(rt)[j]-1];
                    REAL(x)[j+start] = REAL(sq)[INTEGER(rt)[j]-1];
                }
                UNPROTECT(4); // st, dst, sq, rt
            } else {
                if (!R_FINITE(REAL(x)[i]) || !R_FINITE(REAL(x)[i-1])) { i++; continue; }
                // hack to skip checking Inf=Inf, -Inf=-Inf, NA=NA and NaN=NaN... using unsigned int
                if (REAL(x)[i-1]-REAL(x)[i] > REAL(tol)[0] || array[i] == array[i-1]) { i++; continue; }
                start = i-1;
                i++;
                while(i < n && REAL(x)[i-1] - REAL(x)[i] < REAL(tol)[0]) { i++; }
                end = i-1;
                i++;
                if (end-start+1 == 1) continue;
                PROTECT(st = allocVector(INTSXP, end-start+1));
                PROTECT(sq = allocVector(REALSXP, end-start+1));
                // To investigate: probably a simple bubble sort or shell sort may be quicker on groups with < 10 items than a 3-pass radix?
                // Can't rely on R's base radix order because even if you've two items in group and one of them is 3 and the other is 1e6, then it won't work!
                // Just doing this gives 4x speed-up under small group sizes. (from 37 to 6-9 seconds)
                if (end-start+1 == 2) {
                    // avoid radix sort on 2 items
                    if (INTEGER(order)[start] > INTEGER(order)[end]) {
                        // then just swap
                        INTEGER(st)[0] = INTEGER(order)[start];
                        INTEGER(order)[start] = INTEGER(order)[end];
                        INTEGER(order)[end] = INTEGER(st)[0];
                
                        REAL(sq)[0] = REAL(x)[start];
                        REAL(x)[start] = REAL(x)[end];
                        REAL(x)[end] = REAL(sq)[0];
                    }
                    UNPROTECT(2); // st, sq
                    continue;
                }
                for (j=0; j<end-start+1; j++) {
                    INTEGER(st)[j] = INTEGER(order)[j+start];
                    REAL(sq)[j] = REAL(x)[j+start];
                }
                PROTECT(dst = duplicate(st)); // have to duplicate to pass to fastradixint as it modifies by reference
                PROTECT(rt = fastradixint(dst, ridx, dec));
                for (j=0; j<end-start+1; j++) {
                    INTEGER(order)[j+start] = INTEGER(st)[INTEGER(rt)[j]-1];
                    REAL(x)[j+start] = REAL(sq)[INTEGER(rt)[j]-1];
                }
                UNPROTECT(4); // st, dst, sq, rt
            }
        }
        UNPROTECT(2); // ridx, dec
    }
    UNPROTECT(3); // xtmp, order, ordertmp
    if (LOGICAL(return_index)[0]) return(order); 
    return(x);
}

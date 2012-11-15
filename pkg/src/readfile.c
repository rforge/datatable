#include <R.h>
#define USE_RINTERNALS
#include <Rinternals.h>
#include <Rdefines.h>
#include <fcntl.h>
#include <stdio.h>

// TO DO: secondary separator for list() columns, such as columns 11 and 12 in BED.
// TO DO: missing values (on return value of fscanf rescan, but in most cases ok first time)
// TO DO: loop fscanf in batches of 32 (or 128) to remove column limit
// TO DO: test if increasing p from 32 to 128 decreases performance for 10 columns.

extern int sizes[];
SEXP readfile(SEXP fnam, SEXP formatarg, SEXP types, SEXP skip, SEXP estnarg)
{
    SEXP thiscol, ans;
    R_len_t i, protecti=0, nrow=0, ncol, estn;
    int size[32], thistype, nc=0, charcol[32];
    char *buffer[32];
    char *p[32], *format;
    if (LENGTH(types)>32) error("Currently limited to 32 columns just for dev. Will be increased.");
    for (i=0; i<32; i++) p[i] = 0;
    FILE *f=fopen(CHAR(STRING_ELT(fnam,0)),"r");
    if (f == NULL) error("file not found");
    estn = INTEGER(estnarg)[0];
    ncol = LENGTH(types);
    ans=PROTECT(allocVector(VECSXP,ncol));  // TO DO, could over-allocate here directly, or maybe safer to go via alloccol.
    protecti++;
    for (i=0; i<ncol; i++) {
        thistype = TYPEOF(VECTOR_ELT(types,i));
        thiscol = PROTECT(allocVector(thistype,estn));
        protecti++;
        SET_TRUELENGTH(thiscol, estn);
        SET_VECTOR_ELT(ans,i,thiscol);
        if (thistype == STRSXP) {
            charcol[nc] = i;
            p[i] = buffer[nc] = malloc(sizeof(char)*1024);   // TO DO tie in %1024s and return value from fscanf
            size[i] = 0;
            nc++;
        } else {
            p[i] = (char *)DATAPTR(thiscol);
            size[i] = sizes[thistype];
        }
    }
    for (i=0; i<INTEGER(skip)[0]; i++) while (fgetc(f) != '\n');
    format = (char *)CHAR(STRING_ELT(formatarg,0));
    while (fscanf(f, format, p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],            // see note (*) below
                             p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15],
                             p[16],p[17],p[18],p[19],p[20],p[21],p[22],p[23],
                             p[24],p[25],p[26],p[27],p[28],p[29],p[30],p[31]) != EOF) {
        for (i=0; i<ncol; i++) p[i]+=size[i];
        for (i=0; i<nc; i++) SET_STRING_ELT(VECTOR_ELT(ans, charcol[i]), nrow, mkChar(buffer[i]));
        nrow++;
    }
    fclose(f);
    for (i=0; i<ncol; i++) SETLENGTH(VECTOR_ELT(ans,i), nrow);
    UNPROTECT(protecti);
    for (i=0; i<nc; i++) free(buffer[i]);
    return(ans);
}

/*
(*) (Often) unused and zero'd p[*] is deliberate
It isn't possible in C to construct a vargs and pass it to fscanf in C or C99. A '...' can
be passed on, but not created unless we depend on a specialist library or go to asm.
Living with a, say, 32 column limit, seems fine for very many common scientific and business file formats. 
Assumption is that fscanf is optimised internally for navigating the format, and is faster than character by
character code with branching and function calls at the C level (such as in R's scan.c).
*/



#include <R.h>
#define USE_RINTERNALS
#include <Rinternals.h>
#include <Rdefines.h>
//#include <sys/mman.h>
#include <fcntl.h>

int sizes[100];  // max appears to be FUNSXP = 99, see Rinternals.h
SEXP SelfRefSymbol;

SEXP keepattr(SEXP to, SEXP from);
SEXP growVector(SEXP x, R_len_t newlen);
SEXP allocNAVector(SEXPTYPE type, R_len_t n);

void setSizes() {
    // called by init.c
    int i;
    for (i=0;i<100;i++) sizes[i]=0;
    // only these types are currently allowed as column types :
    sizes[INTSXP] = sizeof(int);     // integer and factor
    sizes[LGLSXP] = sizeof(int);     // logical
    sizes[REALSXP] = sizeof(double); // numeric
    sizes[STRSXP] = sizeof(SEXP *);  // character
    sizes[VECSXP] = sizeof(SEXP *);  // a column itself can be a list()
    for (i=0;i<100;i++) {
        if (sizes[i]>8) error("Type %d is sizeof() greater than 8 bytes on this machine. We haven't tested on any architecture greater than 64bit, yet.", i);
        // One place we need the largest sizeof (assumed to be 8 bytes) is the working memory malloc in reorder.c
    }
    SelfRefSymbol = install(".internal.selfref");
}
#define SIZEOF(x) sizes[TYPEOF(x)]


SEXP dogroups(SEXP dt, SEXP dtcols, SEXP groups, SEXP grpcols, SEXP jiscols, SEXP grporder, SEXP order, SEXP starts, SEXP lens, SEXP jexp, SEXP env, SEXP lhs, SEXP newnames, SEXP verbose)
{
    R_len_t i, j, k, rownum, ngrp, njval=0, ngrpcols, ansloc=0, maxn, estn=-1, r, thisansloc, grpn, thislen, igrp, size, vlen;
    int protecti=0;
    SEXP names, names2, bynames, ans=NULL, jval, thiscol, SD, BY, N, iSD, rownames, s, targetcol, RHS, listwrap, target;
    SEXP *nameSyms;
    Rboolean wasvector, firstalloc=FALSE;
    
    if (TYPEOF(order) != INTSXP) error("Internal error: order not integer");
    //if (TYPEOF(starts) != INTSXP) error("Internal error: starts not integer");
    //if (TYPEOF(lens) != INTSXP) error("Internal error: lens not integer");
    // starts can now be NA (<0): if (INTEGER(starts)[0]<0 || INTEGER(lens)[0]<0) error("starts[1]<0 or lens[1]<0");
    if (!isNull(jiscols) && length(order)) error("Internal error: jiscols not NULL but o__ has length");
    if(!isEnvironment(env)) error("’env’ should be an environment");
    ngrp = length(starts);  // the number of groups  (nrow(groups) will be larger when by)
    ngrpcols = length(grpcols);
    SD = findVar(install(".SD"), env);
    BY = findVar(install(".BY"), env);
    N = findVar(install(".N"), env);
    iSD = findVar(install(".iSD"), env);  // 1-row and possibly no cols (if no i variables are used via JIS)
    
    // fetch rownames of .SD.  rownames[1] is set to -thislen for each group, in case .SD is passed to
    // non data.table aware package that uses rownames
    for (s = ATTRIB(SD); s != R_NilValue && TAG(s)!=R_RowNamesSymbol; s = CDR(s));
    // getAttrib0 basically but that's hidden in attrib.c
    if (s==R_NilValue) error("row.names attribute of .SD not found");
    rownames = CAR(s);
    if (!isInteger(rownames) || LENGTH(rownames)!=2 || INTEGER(rownames)[0]!=NA_INTEGER) error("row.names of .SD isn't integer length 2 with NA as first item; i.e., .set_row_names(). [%s %d %d]",type2char(TYPEOF(rownames)),LENGTH(rownames),INTEGER(rownames)[0]);
    
    // fetch names of .SD and prepare symbols. In case they are copied-on-write by user assigning to those variables
    // using <- in j (which is valid, useful and tested), they are repointed to the .SD cols for each group.
    names = getAttrib(SD, R_NamesSymbol);
    if (length(names) != length(SD)) error("length(names)!=length(SD)");
    nameSyms = Calloc(length(names), SEXP);
    if (!nameSyms) error("Calloc failed to allocate %d nameSyms in dogroups",length(names));
    for(i = 0; i < length(SD); i++) {
        if (SIZEOF(VECTOR_ELT(SD, i))==0)
            error("Type %d in .SD column %d", TYPEOF(VECTOR_ELT(SD, i)), i);
        nameSyms[i] = install(CHAR(STRING_ELT(names, i)));
    }

    if (length(iSD)!=length(jiscols)) error("length(iSD)[%d] != length(jiscols)[%d]",length(iSD),length(jiscols));
    
    bynames = getAttrib(BY, R_NamesSymbol);
    if (isNull(jiscols) && (length(bynames)!=length(groups) || length(bynames)!=length(grpcols))) error("!length(bynames)[%d]==length(groups)[%d]==length(grpcols)[%d]",length(bynames),length(groups),length(grpcols));
    // No .BY when bywithoutby.  TO DO: would it be useful to include it there too?
    for(i = 0; i < length(BY); i++) {
        // by vars can be used by name or via .BY
        defineVar(install(CHAR(STRING_ELT(bynames,i))),VECTOR_ELT(BY,i),env);
        if (TYPEOF(VECTOR_ELT(BY,i)) != TYPEOF(VECTOR_ELT(groups,i)))
            error("Type mismatch between .BY and groups, column %d",i);
        if (SIZEOF(VECTOR_ELT(BY,i))==0)
            error("Type %d in by column %d", TYPEOF(VECTOR_ELT(BY, i)), i);
    }
    
    PROTECT(listwrap = allocVector(VECSXP, 1));
    protecti++;

    ansloc = 0;
    for(i=0; i<ngrp; i++) {
        if (INTEGER(starts)[i] == 0 && (i>0 || !isNull(lhs))) continue;
        if (INTEGER(starts)[i] == NA_INTEGER && !isNull(lhs)) continue;
        grpn = INTEGER(lens)[i];
        INTEGER(N)[0] = INTEGER(starts)[i] == NA_INTEGER ? 0 : grpn;
        // .N is number of rows matched to ( 0 even when nomatch is NA)
        
        for (j=0; j<length(iSD); j++) {   // either this or the next for() will run, not both
            size = SIZEOF(VECTOR_ELT(iSD,j));
            memcpy((char *)DATAPTR(VECTOR_ELT(iSD,j)),
                   (char *)DATAPTR(VECTOR_ELT(groups,INTEGER(jiscols)[j]-1))+i*size,
                   size);
        }
        igrp = length(grporder) ? INTEGER(grporder)[INTEGER(starts)[i]-1]-1 : (isNull(jiscols) ? INTEGER(starts)[i]-1 : i);
        for (j=0; j<length(BY); j++) {
            size = SIZEOF(VECTOR_ELT(BY,j));
            memcpy((char *)DATAPTR(VECTOR_ELT(BY,j)),
                   (char *)DATAPTR(VECTOR_ELT(groups,j))+igrp*size,
                   size);
        }
        if (INTEGER(starts)[i] == NA_INTEGER) {
            for (j=0; j<length(SD); j++) {
                switch (TYPEOF(VECTOR_ELT(SD, j))) {
                case LGLSXP :
                    LOGICAL(VECTOR_ELT(SD,j))[0] = NA_LOGICAL;
                    break;
                case INTSXP :
                    INTEGER(VECTOR_ELT(SD,j))[0] = NA_INTEGER;
                    break;
                case REALSXP :
                    REAL(VECTOR_ELT(SD,j))[0] = NA_REAL;
                    break;
                case STRSXP :
                    SET_STRING_ELT(VECTOR_ELT(SD,j),0,NA_STRING);
                    break;
                default:
                    error("Logical error. Type of column should have been checked by now");
                }
            }
            grpn = 1;  // TO DO: it is anyway?
        } else {
            if (length(order)==0) {
                rownum = INTEGER(starts)[i]-1;
                for (j=0; j<length(SD); j++) {
                    size = SIZEOF(VECTOR_ELT(SD,j));
                    memcpy((char *)DATAPTR(VECTOR_ELT(SD,j)),
                       (char *)DATAPTR(VECTOR_ELT(dt,INTEGER(dtcols)[j]-1))+rownum*size,
                       grpn*size);
                }
            } else {
                for (k=0; k<grpn; k++) {
                    rownum = INTEGER(order)[ INTEGER(starts)[i]-1 + k ] -1;
                    for (j=0; j<length(SD); j++) {
                        size = SIZEOF(VECTOR_ELT(SD,j));
                        memcpy((char *)DATAPTR(VECTOR_ELT(SD,j)) + k*size,
                           (char *)DATAPTR(VECTOR_ELT(dt,INTEGER(dtcols)[j]-1)) + rownum*size,
                           size);
                    }
                }
            }
        }
        
        INTEGER(rownames)[1] = -grpn;  // the .set_row_names() of .SD. Not .N when nomatch=NA and this is a nomatch
        for (j=0; j<length(SD); j++) {
            LENGTH(VECTOR_ELT(SD,j)) = grpn;
            defineVar(nameSyms[j], VECTOR_ELT(SD, j), env);
            // In case user's j assigns to the columns names (env is static) (tests 387 and 388)
            // nameSyms pre-stored to save repeated install() for efficiency.
        }
        PROTECT(jval = eval(jexp, env));
        if (!length(jval)) {
            // j may be a plot or other side-effect only
            UNPROTECT(1);
            continue;
        }
        if ((wasvector = isVectorAtomic(jval) || (isSymbol(jexp) && strcmp(CHAR(PRINTNAME(jexp)),".SD")!=0))) {
            // Prior to 1.8.1 the wrap with list() was done up in R after the first group had tested.
            // This wrap is done to make the code below easier to loop through. listwrap avoids allocating for each group 
            SET_VECTOR_ELT(listwrap,0,jval);
            jval = listwrap;
        } else {
            if (!isVector(jval)) error("j must evaluate to vector or list");
            for (j=0; j<length(jval); j++) {
                thiscol = VECTOR_ELT(jval,j);
                if (!isNull(thiscol) && (!isVector(thiscol) || isFrame(thiscol) || isArray(thiscol) ))
                    error("All items in j=list(...) should be atomic vectors or lists. If you are trying something like j=list(.SD,newcol=mean(colA)) then use := by group instead (much quicker), or cbind or merge afterwards.");
            }
        }
        // TO DO: check if direct NULL in user j is auto wrapped to make list(NULL) (length 1)
        if (!isNull(lhs)) {
            targetcol = VECTOR_ELT(dt,INTEGER(lhs)[0]-1);
            RHS = VECTOR_ELT(jval,0);
            if (isNull(targetcol)) {
                // first time adding to new column
                if (isNull(RHS)) error("RHS is NULL when grouping :=. Makes no sense to delete a column by group. Perhaps use an empty vector instead.");
                if (TRUELENGTH(dt) < INTEGER(lhs)[0]) error("Internal error: Trying to add new column by reference but tl is full; alloc.col should have run first at R level before getting to this point in dogroups");
                SET_VECTOR_ELT(dt,INTEGER(lhs)[0]-1,allocNAVector(TYPEOF(RHS), LENGTH(VECTOR_ELT(dt,0))));
                SET_STRING_ELT(getAttrib(dt, R_NamesSymbol), INTEGER(lhs)[0]-1, STRING_ELT(newnames,0));
                LENGTH(getAttrib(dt, R_NamesSymbol))++;
                LENGTH(dt)++;
                targetcol = VECTOR_ELT(dt,INTEGER(lhs)[0]-1);
            }
            if (TYPEOF(targetcol)!=TYPEOF(RHS)) error("Type of RHS ('%s') must match LHS ('%s'). To check and coerce would impact performance too much for the fastest cases. Either change the type of the target column, or coerce the RHS of := yourself (e.g. by using 1L instead of 1)", type2char(TYPEOF(RHS)), type2char(TYPEOF(targetcol)));
            size = SIZEOF(targetcol);
            vlen = length(RHS);
            if (length(order)==0) {
                rownum = INTEGER(starts)[i]-1;
                switch (TYPEOF(targetcol)) {
                case STRSXP :
                    for (r=0; r<grpn; r++)
                        SET_STRING_ELT(targetcol, rownum+r, STRING_ELT(RHS, r%vlen));
                    break;
                case VECSXP :
                    for (r=0; r<grpn; r++)
                        SET_VECTOR_ELT(targetcol, rownum+r, STRING_ELT(RHS, r%vlen));
                    break;
                default :
                    for (r=0; r<(grpn/vlen); r++) {
                        memcpy((char *)DATAPTR(targetcol) + (rownum+r*vlen)*size,
                               (char *)DATAPTR(RHS),
                               vlen * size);
                    }
                    memcpy((char *)DATAPTR(targetcol) + (rownum+r*vlen)*size,
                           (char *)DATAPTR(RHS),
                           (grpn%vlen) * size);
                }
            } else {
                switch (TYPEOF(targetcol)) {
                case STRSXP :
                    for (k=0; k<grpn; k++) {
                        rownum = INTEGER(order)[ INTEGER(starts)[i]-1 + k ] -1;
                        SET_STRING_ELT(targetcol, rownum, STRING_ELT(RHS, k%vlen));
                    }
                    break;
                case VECSXP :
                    for (k=0; k<grpn; k++) {
                        rownum = INTEGER(order)[ INTEGER(starts)[i]-1 + k ] -1;
                        SET_VECTOR_ELT(targetcol, rownum, VECTOR_ELT(RHS, k%vlen));
                    }
                    break;
                case INTSXP :
                    for (k=0; k<grpn; k++) {
                        rownum = INTEGER(order)[ INTEGER(starts)[i]-1 + k ] -1;
                        INTEGER(targetcol)[rownum] = INTEGER(RHS)[k%vlen];
                    }
                    break;
                case REALSXP :
                    for (k=0; k<grpn; k++) {
                        rownum = INTEGER(order)[ INTEGER(starts)[i]-1 + k ] -1;
                        REAL(targetcol)[rownum] = REAL(RHS)[k%vlen];
                    }
                    break;
                default :
                    error("Unsupported type");
                }
            }
            UNPROTECT(1);
            continue;
        }
        maxn = 0;
        if (length(jval)>1 || length(VECTOR_ELT(jval,0))) {
            if (njval==0) njval = length(jval);   // for first group, then the rest must conform
            if (njval != length(jval)) error("j doesn't evaluate to the same number of columns for each group");  // this would be a problem even if we unlisted afterwards. This way the user finds out earlier though so he can fix and rerun sooner.
            for (j=0; j<njval; j++) {
                k = length(VECTOR_ELT(jval,j));  // might be NULL, so length not LENGTH
                maxn = k>maxn ? k : maxn;
            }
        } // else j was NULL or data.table(NULL) etc (tests 361-364)
        if (maxn==0) {
            UNPROTECT(1);   // jval
            continue;
        }
        if (ansloc + maxn > estn) {
            if (estn == -1) {
                // Given first group and j's result on it, make a good guess for size of result required.
                // This was 'byretn' in R in v1.8.0, now here in C from v1.8.1 onwards.
                if (grpn==0)
                    estn = maxn = 0;   // empty case e.g. test 184. maxn is 1 here due to sum(integer()) == 0L
                else if (maxn==1) // including when grpn==1 we default to assuming it's an aggregate
                    estn = LENGTH(starts);
                    // Common case 1 : j is a list of simple aggregates i.e. list of atoms only
                else if (maxn >= grpn) {
                    estn = 0;
                    for (j=0;j<LENGTH(lens);j++) estn+=INTEGER(lens)[j];
                    // Common case 2 : j returns as many rows as there are in the group (maybe a join)
                    // TO DO: this might over allocate if first group has 1 row and j is actually a single row aggregate
                    //        in cases when we're not sure could wait for the first few groups before deciding.
                } else  // maxn < grpn
                    estn = maxn * LENGTH(starts);
                    // Common case 3 : head or tail of .SD perhaps
                if (estn<maxn) estn=maxn;  // if the result for the first group is larger than the table itself(!) Unusual case where a join is being done in j via .SD and the 1-row table is an edge case of bigger picture.
                PROTECT(ans = allocVector(VECSXP, ngrpcols + njval));
                protecti++;
                firstalloc=TRUE; 
                for(j=0; j<ngrpcols; j++) {
                    thiscol = VECTOR_ELT(groups, INTEGER(grpcols)[j]-1);
                    SET_VECTOR_ELT(ans, j, keepattr(allocVector(TYPEOF(thiscol), estn), thiscol));
                }
                for(j=0; j<njval; j++) {
                    thiscol = VECTOR_ELT(jval, j);
                    if (isNull(thiscol))
                        error("Column %d of j's result for the first group is NULL. We rely on the column types of the first result to decide the type expected for the remaining groups (and require consistency). NULL columns are acceptable for later groups (and those are replaced with NA of appropriate type and recycled) but not for the first. Please use a typed empty vector instead, such as integer() or numeric().", j+1);
                    if (LOGICAL(verbose)[0] && !isNull(getAttrib(thiscol, R_NamesSymbol))) {
                        if (wasvector) {
                            Rprintf("j appears to be a named vector. The same names will likely be created over and over again for each group and slow things down. Try and pass a named list (which data.table optimizes) or an unnamed list() instead.");
                        } else {
                            Rprintf("Column %d of j is a named vector (each item down the rows is named, somehow). Please remove those names for efficiency (to save creating them over and over for each group). They are ignored anyway.", j+1);
                        }
                    }
                    setAttrib(thiscol, R_NamesSymbol, R_NilValue);    // If not cleared, test 276 would fail
                    setAttrib(thiscol, R_DimSymbol, R_NilValue);      // Probably superfluous since non-atomic was
                    setAttrib(thiscol, R_DimNamesSymbol, R_NilValue);  // caught earlier.
                    SET_VECTOR_ELT(ans, ngrpcols+j, keepattr(allocVector(TYPEOF(thiscol), estn), thiscol));
                    // See growVector for comments about keepattr.
                }
                names = getAttrib(jval, R_NamesSymbol);
                if (!isNull(names)) {
                    if (LOGICAL(verbose)[0]) Rprintf("The result of j is a named list. It's very inefficient to create the same names over and over again for each group. When j=list(...), any names are detected, removed and put back after grouping has completed, for efficiency. Using j=transform(), for example, prevents that speedup (consider changing to :=). This message may be upgraded to warning in future.\n");  // e.g. test 104 has j=transform().  
                    // names of result come from the first group and the names of remaining groups are ignored (all that matters for them is that the number of columns (and their types) match the first group.
                    names2 = PROTECT(allocVector(STRSXP,ngrpcols+njval));
                    protecti++;
                    for (j=0; j<ngrpcols; j++) SET_STRING_ELT(names2, j, STRING_ELT(bynames,j));
                    for (j=0; j<njval; j++) SET_STRING_ELT(names2, ngrpcols+j, STRING_ELT(names,j));
                    setAttrib(ans, R_NamesSymbol, names2);
                    // setAttrib(SD, R_NamesSymbol, R_NilValue); // so that lapply(.SD,mean) is unnamed from 2nd group on
                }
            } else {
                estn = ((double)ngrp/i)*1.1*(ansloc+maxn);
                if (LOGICAL(verbose)[0]) Rprintf("dogroups: growing from %d to %d rows\n", length(VECTOR_ELT(ans,0)), estn);
                for (j=0; j<ngrpcols; j++) SET_VECTOR_ELT(ans, j, growVector(VECTOR_ELT(ans,j), estn));
                for (j=0; j<njval; j++) SET_VECTOR_ELT(ans, j+ngrpcols, growVector(VECTOR_ELT(ans,j+ngrpcols), estn));
            }
        }
        // Now copy jval into ans ...
        for (j=0; j<ngrpcols; j++) {
            size = SIZEOF(VECTOR_ELT(groups, j));
            for (r=0; r<maxn; r++) {
                memcpy((char *)DATAPTR(VECTOR_ELT(ans,j)) + (ansloc+r)*size,  // TO DO: a switched assign here?
                    (char *)DATAPTR(VECTOR_ELT(groups,j)) + igrp*size,   // generations a concern but this one ok I think
                    1 * size);
            }
        }
        for (j=0; j<njval; j++) {
            thisansloc = ansloc;
            thislen = length(VECTOR_ELT(jval,j));
            target = VECTOR_ELT(ans, j+ngrpcols);
            if (thislen == 0) {
                // including NULL and typed empty vectors, fill with NA
                // A NULL in the first group's jval isn't allowed; caught above after allocating ans
                switch (TYPEOF(target)) {
                case LGLSXP :
                case INTSXP :
                    for (r=0; r<maxn; r++) INTEGER(target)[thisansloc+r] = NA_INTEGER;
                    break;
                case REALSXP :
                    for (r=0; r<maxn; r++) REAL(target)[thisansloc+r] = NA_REAL;
                    break;
                case STRSXP :
                    for (r=0; r<maxn; r++) SET_STRING_ELT(target,thisansloc+r,NA_STRING);
                    break;
                default:
                    error("Logical error. Type of column should have been checked by now");
                }
                continue;
            }
            // TO DO: remove this restriction and add extra memcpy for last chunk later below ...
            if (maxn%thislen != 0) error("maxn (%d) is not exact multiple of this j column's length (%d)",maxn,thislen);
            if (TYPEOF(VECTOR_ELT(jval, j)) != TYPEOF(VECTOR_ELT(ans, j+ngrpcols)))
                error("columns of j don't evaluate to consistent types for each group: result for group %d has column %d type '%s' but expecting type '%s'", i+1, j+1, type2char(TYPEOF(VECTOR_ELT(jval, j))), type2char(TYPEOF(VECTOR_ELT(ans, j+ngrpcols))));
            // TO DO : a switch would be neater here ...
            if (TYPEOF(VECTOR_ELT(jval,j))==STRSXP) {
                for (r=0; r<maxn; r++) {
                    SET_STRING_ELT(VECTOR_ELT(ans,j+ngrpcols), thisansloc, STRING_ELT(VECTOR_ELT(jval,j),r%thislen));
                    thisansloc++;
                }
                continue;
                // TO DO: Replace SET_STRING_ELT and SET_VECTOR_ELT with direct CHK and CHECK_OLD_TO_NEW calls,
                // once per item in jval, then the rest can be memcpy'd after?. But, it seems we do need to
                // ensure objects are aged.
            }
            if (TYPEOF(VECTOR_ELT(jval,j))==VECSXP) {
                for (r=0; r<maxn; r++) {
                    SET_VECTOR_ELT(VECTOR_ELT(ans,j+ngrpcols), thisansloc, VECTOR_ELT(VECTOR_ELT(jval,j),r%thislen));
                    thisansloc++;
                }
                continue;
            }
            // else integer or real
            size = SIZEOF(VECTOR_ELT(jval,j));
            for (r=0; r<(maxn/thislen); r++) {
                memcpy((char *)DATAPTR(VECTOR_ELT(ans,j+ngrpcols)) + thisansloc*size,
                   (char *)DATAPTR(VECTOR_ELT(jval,j)),
                   thislen * size);
                thisansloc += thislen;
            }
            // TO DO : extra memcpy needed on this line if we allow non exact multiples
        }
        ansloc += maxn;
        if (firstalloc) {
            protecti++;          //  remember the first jval. If we UNPROTECTed now, we'd unprotect 
            firstalloc = FALSE;  //  ans. The first jval is needed to create the right size and type of ans.
            // TO DO: could avoid this last 'if' by adding a dummy PROTECT after first alloc for this UNPROTECT(1) to do. 
        }
        else UNPROTECT(1);  // the jval. Don't want them to build up. The first jval can stay protected till the end ok.
    }
    if (isNull(lhs) && ans!=NULL) {
        if (ansloc < LENGTH(VECTOR_ELT(ans,0))) {
            if (LOGICAL(verbose)[0]) Rprintf("Wrote less rows (%d) than allocated (%d).\n",ansloc,LENGTH(VECTOR_ELT(ans,0)));
            for (j=0; j<length(ans); j++) LENGTH(VECTOR_ELT(ans,j)) = ansloc;
            // TO DO: set truelength here (uninitialized by R) to save growing next time on insert() 
        }
    } else ans = R_NilValue;
    UNPROTECT(protecti);
    Free(nameSyms);
    return(ans);
}

SEXP keepattr(SEXP to, SEXP from)
{
    // Same as R_copyDFattr in src/main/attrib.c, but that seems not exposed in R's api
    // Only difference is that we reverse from and to in the prototype, for easier calling above
    SET_ATTRIB(to, ATTRIB(from));
    IS_S4_OBJECT(from) ?  SET_S4_OBJECT(to) : UNSET_S4_OBJECT(to);
    SET_OBJECT(to, OBJECT(from));
    return to;
}

SEXP growVector(SEXP x, R_len_t newlen)
{
    // Similar to EnlargeVector in src/main/subassign.c, with the following changes :
    // * replaced switch and loops with one memcpy for INTEGER and REAL, but need to age CHAR and VEC.
    // * no need to cater for names
    // * much shorter and faster
    SEXP newx;
    R_len_t i, len = length(x);
    PROTECT(newx = allocVector(TYPEOF(x), newlen));   // TO DO: R_realloc(?) here?
    switch (TYPEOF(x)) {
    case STRSXP :
        for (i=0; i<len; i++)
            SET_STRING_ELT(newx, i, STRING_ELT(x, i));
            // Using SET_ to ensure objects are aged, rather than memcpy. Perhaps theres a bulk/fast way to age CHECK_OLD_TO_NEW
        break;
    case VECSXP :
        for (i=0; i<len; i++)
            SET_VECTOR_ELT(newx, i, VECTOR_ELT(x, i));
        break;
    default :
        memcpy((char *)DATAPTR(newx), (char *)DATAPTR(x), len*SIZEOF(x));
    }
    // if (verbose) Rprintf("Growing vector from %d to %d items of type '%s'\n", len, newlen, type2char(TYPEOF(x)));
    // Would print for every column if here. Now just up in dogroups (one msg for each table grow).
    keepattr(newx,x);
    UNPROTECT(1);
    return newx;
}



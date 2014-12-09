#include "./include/parms_pc_impl.h"
#include "./include/parms_opt_impl.h"
#if defined(__ICC)
#include <mathimf.h>
#else
#if defined(C99)
#include <tgmath.h>
#else
#include <math.h>
#endif 
#endif 

typedef struct schur_data {
  parms_Operator op;
  int n, schur_start, nrow;//nrow = n - schur-start//n = lsize// in block version, the three variables become point index
  //int *localbsz;//specially for block local preconditioner //changed
  //int nn, schur_start_p;//specially for block method, our nrow = 
  int im, maxits, in_iters;
  MPI_Comm comm;
  double pgfpar[2];
  FLOAT **vv, **hh, *s, *rs, *hcol, *z2, *wk;
  REAL *c;
} *schur_data;

/** Free the memory for struct schur_data.
 *
 *  \param self A preconditioner object.
 *  \return 0 on success.
 */
static int pc_schur_free(parms_PC *self)
{
  schur_data pc_data;
  parms_Operator op;
  int i;

  pc_data = (schur_data)(*self)->data;
  op = pc_data->op;
  parms_OperatorFree(&op);

  //if (pc_data->localbsz) {
    //PARMS_FREE(pc_data->localbsz);//new
  //}


  if (pc_data->nrow) {
    PARMS_FREE(pc_data->z2);
    PARMS_FREE(pc_data->wk);
    for (i = 0; i < pc_data->im+1; i++) {
      PARMS_FREE(pc_data->vv[i]);
    }
  }
  PARMS_FREE(pc_data->vv);
  for (i = 0; i < pc_data->im; i++) {
    PARMS_FREE(pc_data->hh[i]);
  }
  PARMS_FREE(pc_data->hh);
  PARMS_FREE(pc_data->c);
  PARMS_FREE(pc_data->s);
  PARMS_FREE(pc_data->rs);
  PARMS_FREE(pc_data->hcol);
  MPI_Comm_free(&pc_data->comm);
  PARMS_FREE(pc_data);
  (*self)->param->isalloc = false;    
  return 0;
}

/** Dump the Schur preconditioner.
 *
 *  \param self A preconditioner object.
 *  \param v    A viewer.
 *  \return 0 on success.
 */
static int pc_schur_view(parms_PC self, parms_Viewer v)
{
  schur_data pc_data;

  pc_data = (schur_data)self->data;
  parms_OperatorView(pc_data->op, v);
  return 0;
}

/** Set up data for the Schur preconditioner.
 *
 *  Malloc memories for working arrays in the inner GMRES.
 *
 *  \param self A preconditioner object.
 *  \return 0 on success.
 */
static int pc_schur_setup(parms_PC self)
{
  schur_data pc_data;
  parms_Mat A;
  void *diag_mat;
  parms_FactParam param;
  parms_Operator op;
  int nrow, i;

  A =  self->A;

  /* Set parameters for the ILU factorization:
     ILUs in pARMS can perform factorizatoin based on the matrix
     merged implicitly by several submatrices. 
     start       - the location in the merged matrix of the first row
                   index of the submatrix.
     n           - the size of the merged matrix.
     schur_start - the beginning row index of the Schur in the merged
                   matrix 
  */
  param = self->param;
  param->start = 0;
  param->n = A->is->lsize;
  param->schur_start = -1;
  pc_data = (schur_data)self->data;

  /* Get diagonal part of the matrix */
  parms_MatGetDiag(A, &diag_mat);

//  pc_data->localbsz = NULL;//new

  /* Perform local ILU-type factorization */
  parms_PCILU(self, param, diag_mat, &op);

  pc_data->op = op;

  MPI_Comm_dup(A->is->comm, &pc_data->comm);

  /* get Krylov  subspace size */
  pc_data->im = param->ipar[4];
  //printf("pc_data->im = %d\n",pc_data->im);
  if (pc_data->im == 0) {
    pc_data->im = 5;
  }

  /* maximum inner iteration counts */
  pc_data->maxits = param->ipar[5];
  //printf("pc_data->maxits = %d\n",pc_data->maxits);
  if (pc_data->maxits == 0) {
    pc_data->maxits = 5;
  }

  /* 
     malloc memory for krylov subspace basis vv and Hessenberg matrix
  */
  PARMS_NEWARRAY(pc_data->vv, pc_data->im+1);


  /* Set the size of the matrix */
  pc_data->n = param->n;
  // printf("pc_data->n = %d\n",pc_data->n);

  /* Set the beginning location of the Schur complement */
  pc_data->schur_start = parms_OperatorGetSchurPos(op);//block version??

  //printf("pc_data->schur_start = %d\n",pc_data->schur_start);
/*   pc_data->schur_start = A->is->schur_start; */

  /* The size of local Schur complement */
  nrow = pc_data->n - pc_data->schur_start;
  pc_data->nrow = nrow;

/* initialise the localbsz, in the point version code, we set it to NULL*/
//  pc_data->localbsz = NULL;


  /* Allocate memories for auxiliary arrays. */
  if(nrow) {
    for(i = 0; i < pc_data->im+1; i++) {
      PARMS_NEWARRAY(pc_data->vv[i], nrow);
    }
    PARMS_NEWARRAY(pc_data->z2, nrow);
    PARMS_NEWARRAY(pc_data->wk, nrow);
  }
  else {
    pc_data->z2 = NULL;
    pc_data->wk = NULL;
    pc_data->vv[0] = NULL;
  }

  PARMS_NEWARRAY(pc_data->hh, pc_data->im);
  for(i = 0; i < pc_data->im; i++) {
    PARMS_NEWARRAY(pc_data->hh[i], pc_data->im+1);
  }
  
  PARMS_NEWARRAY(pc_data->c,    pc_data->im);
  PARMS_NEWARRAY(pc_data->s,    pc_data->im);
  PARMS_NEWARRAY(pc_data->rs,   pc_data->im+1);
  PARMS_NEWARRAY(pc_data->hcol, pc_data->im+1);

  return 0;
}

/** Apply the preconditioner to the vector y.
 *
 *  This preconditioner is actually the lsch_xx preconditioners in old
 *  version of pARMS. 
 *
 *  \param self A preconditioner object.
 *  \param y    A right-hand-side vector.
 *  \param x    Solution vector.
 *  \return 0 on success.
 */
static int pc_schur_apply(parms_PC self, FLOAT *y, FLOAT *x)
{
  /*--------------------------------------------------------------------
    APPROXIMATE LU-SCHUR LEFT PRECONDITONER
    *--------------------------------------------------------------------
    Schur complement left preconditioner.  This is a preconditioner for 
    the global linear system which is based on solving approximately the
    Schur  complement system. More  precisely, an  approximation to the
    local Schur complement  is obtained (implicitly) in  the form of an
    LU factorization  from the LU  factorization  L_i U_i of  the local
    matrix A_i. Solving with this approximation amounts to simply doing
    the forward and backward solves with the bottom part of L_i and U_i
    only (corresponding to the interface variables).  This is done using
    a special version of the subroutine lusol0 called lusol0_p. Then it
    is possible to  solve  for an  approximate  Schur complement system
    using GMRES  on  the  global  approximate Schur  complement  system
    (which is preconditionied  by  the diagonal  blocks  represented by
    these restricted LU matrices).
    --------------------------------------------------------------------
    Coded by Y. Saad - Aug. 1997 - updated Nov. 14, 1997.
    C version coded by Zhongze Li, Jan. 2001, updated Sept, 17th, 2001
    Revised by Zhongze Li, June 1st, 2006.
    --------------------------------------------------------------------
  */
  schur_data     pc_data;
  parms_Operator op;
  parms_Map      is;
  parms_Mat      A;
  parms_FactParam param;
  MPI_Comm       comm;
  int schur_start,nrow, im,i,jj,i1,k,k1,its,j,ii,maxits,incx=1,ierr;
  int if_in_continue,if_out_continue;
  FLOAT *z2, *wk;
  FLOAT **vv, **hh, *s, *rs, *hcol, t1, alpha;
  REAL *c;
  REAL  eps,eps1,t,ro,tloc;  
  
#if defined(DBL_CMPLX)
   FLOAT rot;
#else
    REAL gam;    
#endif    
  static int iters = 0;

  /* retrieve the specific data structure for Schur based preconditioners  */ 
  pc_data = (schur_data)self->data;

  /* get the matrix A */
  A = self->A;

  is = A->is;

//printf("In schur_apply A = %d\n", A );getchar();
//outputcsmatpa(A->aux_data,"aux_data",1);
//output_dblvectorpa("y_in_schur_apply",y,0, pc_data->nrow);//getchar();
  /* get the dimension of the local matrix */
  /* n = pc_data->n; */

  /* get the beginning location of the Schur complement */
  schur_start = pc_data->schur_start;

  /* get the size of the Schur complement */
  nrow = pc_data->nrow;

  /* get krylov space size */
  im = pc_data->im;

  /* get maximal iteration number */
  maxits = pc_data->maxits;

  /* get tolerance */
  param = self->param;
  eps = param->pgfpar[0];
  eps1 = eps;

  /* get communicator */
  comm = pc_data->comm;

  /* get working arrays */
  vv   = pc_data->vv;
  hh   = pc_data->hh;
  c    = pc_data->c;
  s    = pc_data->s;
  rs   = pc_data->rs;
  hcol = pc_data->hcol;
  z2   = pc_data->z2;
  wk   = pc_data->wk;

  /* retrieve the operator */
  op = pc_data->op;
  /* compute the right-hand side of Schur Complement System
     it turns out that  this is the bottom part of inv(A_i)*rhs
  */

  parms_OperatorLsol(op, y, x);//vbarms Lsol is untested 

  GCOPY(nrow, &x[schur_start], incx, wk, incx);
  parms_OperatorInvS(op, &x[schur_start], vv[0]);//vbarms invs is untested 

  for (i = 0; i < nrow; i++) {
    x[schur_start+i] = 0.0;
  }


  /* solution loop by gmres. */
  if_out_continue = 1;
  its = 0;

  while(if_out_continue) {
#if defined(DBL_CMPLX)  
    t1 = GDOTC(nrow, vv[0], incx, vv[0], incx);
    t = creal(t1);
#else
    t = GDOT(nrow, vv[0], incx, vv[0], incx);
#endif
    MPI_Allreduce(&t, &ro, 1, MPI_DOUBLE, MPI_SUM, comm);
    ro = sqrt(ro);
    if(fabs(ro - ZERO) <= EPSILON) { 
      ierr = -1;
      break;
    }

    t1 = 1.0/ro;

    GSCAL(nrow, t1, vv[0], incx);

    if(its == 0) eps1 = eps*ro;
    /* initialize 1-st term of rhs of hessenberg system. */
#if defined(DBL_CMPLX)
    rs[0] = ro + 0.0*I;
#else
    rs[0] = ro;
#endif
    i = -1;
    if_in_continue = 1;
    while(if_in_continue) {
      ++i;
      ++its;
      i1 = i + 1;
  
      /* 
	 matrix-vector product
	 tmp = (0,y)^t: first fill tmp with zero components from 1 to n
	 then copy rhs into "y" part of tmp
      */
      // printf("is->nint = %d\n", is->nint);
      //printf("is->ninf = %d\n", is->ninf);
      parms_MatVecOffDiag(A, vv[i], z2, schur_start);
      parms_OperatorInvS(op, z2, z2);

      /* result = input + last part of z2 */
      for(j = 0; j < nrow; j++) {
	vv[i1][j] = vv[i][j] + z2[j];
      }

      /* modified gram - schmidt .. */
#if defined(DBL_CMPLX)      
      for(j = 0; j <= i; j++) {
	hcol[j] = GDOTC(nrow, vv[j], incx, vv[i1], incx);
      }
      MPI_Allreduce(hcol, hh[i], i+1, MPI_CMPLX, MPI_CMPLX_SUM, comm);      
#else
      for(j = 0; j <= i; j++) {
	hcol[j] = GDOT(nrow, vv[j], incx, vv[i1], incx);
      }
      MPI_Allreduce(hcol, hh[i], i+1, MPI_DOUBLE, MPI_SUM, comm);      
#endif

      for(j = 0; j <= i; j++) {
	alpha = -hh[i][j];
	GAXPY(nrow, alpha, vv[j], incx, vv[i1], incx);
      }
#if defined(DBL_CMPLX)
      t1 = GDOTC(nrow, vv[i1], incx, vv[i1], incx);
      tloc = creal(t1);
#else
      tloc = GDOT(nrow, vv[i1], incx, vv[i1], incx);
#endif
      MPI_Allreduce(&tloc, &t, 1, MPI_DOUBLE, MPI_SUM, comm);
      t = sqrt(t);
#if defined(DBL_CMPLX)      
      hh[i][i1] = t + 0.0*I;
#else
      hh[i][i1] = t;
#endif
      if(fabs(t - ZERO) > EPSILON) {
	t1 = 1.0/t;
	GSCAL(nrow, t1, vv[i1], incx);
      }
      /* 
	 done with modified gram Schmidt and Arnoldi step..
	 now update factorization of hh 
      */
      if(i != 0) {
	/* perform previous transformations on i-th column of h */
#if defined(DBL_CMPLX)	
	for(k = 1; k <= i; k++) {
	  k1 = k-1;
	  t1 = hh[i][k1];
	  hh[i][k1] = c[k1]*t1 + s[k1]*hh[i][k];
	  hh[i][k] = -conj(s[k1])*t1 + c[k1]*hh[i][k];
	}
#else
	for(k = 1; k <= i; k++) {
	  k1 = k-1;
	  t1 = hh[i][k1];
	  hh[i][k1] = c[k1]*t1 + s[k1]*hh[i][k];
	  hh[i][k] = -s[k1]*t1 + c[k1]*hh[i][k];
	}
#endif
      }
      
#if defined(DBL_CMPLX)
/*-----------get next plane rotation------------ */
      zclartg(hh[i][i], hh[i][i1], &c[i], &s[i], &rot);
      rs[i1] = -conj(s[i])*rs[i];
      rs[i] = c[i]*rs[i];      
      hh[i][i] = rot;
      ro = cabs(rs[i1]);
#else
      gam = sqrt(hh[i][i]*hh[i][i] + hh[i][i1]*hh[i][i1]);
      /* 
	 if gamma is zero then any small value will do ...
	 will affect only residual estimate 
      */
      if(fabs(gam-ZERO) < EPSILON) gam = EPSMAC;
      /* get next plane rotation */
      c[i] = hh[i][i] / gam;
      s[i] = hh[i][i1] / gam;
      rs[i1] = -s[i]*rs[i];
      rs[i] = c[i]*rs[i];
 
      /* determine residual norm and test for convergence */
      hh[i][i] = c[i]*hh[i][i] + s[i]*hh[i][i1];
      ro = fabs(rs[i1]);
#endif

      if(i+1 >= im || ro <= eps1 || its >= maxits)
	if_in_continue = 0;
    }

    /* now compute solution. first solve upper triangular system. */
    for(ii = i; ii >= 0; --ii) {
      t1 = rs[ii];
      for(j = ii+1; j <= i; j++) {
	t1 = t1 - hh[j][ii]*rs[j];
      }
      rs[ii] = t1 / hh[ii][ii];
    }

    /* form linear combination of v(*,i)'s to update solution */
    for(j = 0; j < i+1; j++) {
      GAXPY(nrow, rs[j], vv[j], incx, &x[schur_start], incx);
    }
    /* restart outer loop when necessary */
    if(ro <= eps1) {
      ierr = 0;
      if_out_continue = 0;
    }
    else if(its >= maxits) {
      ierr = 1;
      if_out_continue = 0;
    }
    /* else compute residual vector (from the v's) and continue.. */
    else {
#if defined(DBL_CMPLX)    
      for(j = 0; j <= i; j++) {
	jj= i1-j+1;
	rs[jj-1] = -conj(s[jj-1])*rs[jj];
	rs[jj] = c[jj-1]*rs[jj];
      }
#else
      for(j = 0; j <= i; j++) {
	jj= i1-j+1;
	rs[jj-1] = -s[jj-1]*rs[jj];
	rs[jj] = c[jj-1]*rs[jj];
      }
#endif   
      for(j = 0; j <= i1; j++) {
	t1 = rs[j];
	if(j == 0) t1 = t1-1.0;
	GAXPY(nrow, t1, vv[j], incx, vv[0], incx);
      }
    }
  }
  /* 
     post-processing to compute interior variables..
     compute RHS for last system. similar to Schur iteration
     first get a vector of the form (0,y)^T
  */
  parms_MatVecOffDiag(A, &x[schur_start], z2, schur_start); 

  GCOPY(nrow, wk, incx, &x[schur_start], incx); 
  for (i = 0; i < is->ninf; i++) {
    x[i+is->nint] -= z2[i+is->nint-schur_start]; 
  }
  /* solve the Schur complement system */
  parms_OperatorInvS(op, &x[schur_start], &x[schur_start]);
  /* backward sweep */
  parms_OperatorAscend(op, x, x);

  iters += its;
  pc_data->in_iters = iters;

  return ierr;
}

/** Get the ratio of the number of nonzero entries of the
 *  preconditioning matrix to that of the original matrix.
 *
 *  \param self   A preconditioner.
 *  \param ratio  A pointer to the ratio.      
 *  \return 0 on success.
 */
static int pc_schur_getratio(parms_PC self, double *ratio)
{
    schur_data pc_data;
    parms_Operator op;
    int nnz_mat, nnz_pc;
    //  int gnnz_mat, gnnz_pc;

    long int gnnz_mat, gnnz_pc;
    long int nnz_mat_l, nnz_pc_l;

    pc_data = (schur_data)self->data;
    op = pc_data->op;
    parms_OperatorGetNnz(op, &nnz_mat, &nnz_pc);
    nnz_mat_l = (long int)nnz_mat;
    nnz_pc_l = (long int)nnz_pc;

//    MPI_Allreduce(&nnz_mat, &gnnz_mat, 1, MPI_INT, MPI_SUM,
//                  MPI_COMM_WORLD);
//    MPI_Allreduce(&nnz_pc, &gnnz_pc, 1, MPI_INT, MPI_SUM,
//                  MPI_COMM_WORLD);
    MPI_Allreduce(&nnz_mat_l, &gnnz_mat, 1, MPI_LONG, MPI_SUM,
                  MPI_COMM_WORLD);
    MPI_Allreduce(&nnz_pc_l, &gnnz_pc, 1, MPI_LONG, MPI_SUM,
                  MPI_COMM_WORLD);
    *ratio = (double)gnnz_pc/(double)gnnz_mat;
    return 0;
}

/** Create a Schur-complement based preconditioner.
 *
 *  \param self A preconditioner object.
 *  \return 0 on success.
 */
int parms_PCCreate_Schur(parms_PC self)
{
  schur_data data;

  PARMS_NEW(data);
  self->data = data;
  self->ops->pc_free     = pc_schur_free;
  self->ops->pc_view     = pc_schur_view;
  self->ops->apply    = pc_schur_apply;
  self->ops->setup    = pc_schur_setup;
  self->ops->getratio = pc_schur_getratio;
  return 0;
}

//-----------------------------------------------------------------------------------dividing line-----------------------------------------------------------------------------------

/** Set up data for the Schur preconditioner.
 *
 *  Malloc memories for working arrays in the inner GMRES.
 *
 *  \param self A preconditioner object.
 *  \return 0 on success.
 */
static int pc_schur_b_setup(parms_PC self)
{
  schur_data pc_data;
  parms_Mat A;
  void *diag_mat;
  parms_FactParam param;
  parms_Operator op;
  int nrow, i;
  int *localbsz;

  A =  self->A;


/*typedef struct schur_data {
  parms_Operator op;
  int n, schur_start, nrow;
  int im, maxits, in_iters;
  MPI_Comm comm;
  double pgfpar[2];
  FLOAT **vv, **hh, *s, *rs, *hcol, *z2, *wk;
  REAL *c;
} *schur_data;*/

  /* Set parameters for the ILU factorization:
     ILUs in pARMS can perform factorizatoin based on the matrix
     merged implicitly by several submatrices. 
     start       - the location in the merged matrix of the first row
                   index of the submatrix.
     n           - the size of the merged matrix.
     schur_start - the beginning row index of the Schur in the merged
                   matrix 
  */
  param = self->param;
  param->start = 0;
  param->n = A->is->lsize;//remain the same
  param->schur_start = -1;
  pc_data = (schur_data)self->data;

  /* Get diagonal part of the matrix */
  //A->isin_b_schur = true;
  printf("A->isin_b_schur = %d\n",A->isin_b_schur);
  parms_MatGetDiag(A, &diag_mat);
  //A->isin_b_schur = false;


  parms_bvcsr  b_diag_mat;
  b_diag_mat = (parms_bvcsr)diag_mat;//??
  //localbsz = b_diag_mat->bsz;
  PARMS_NEWARRAY(localbsz,    param->n+1);
  PARMS_MEMCPY(localbsz, b_diag_mat->bsz, param->n+1);
  //printf("b_diag_mat->bsz = %p\n",b_diag_mat->bsz);
  //output_intvectorpa("b_diag_mat_bsz_schur",b_diag_mat->bsz,0, param->n+1);
  //printf("localbsz[n] = %d\n",localbsz[param->n]);
   /* MPI_Barrier(MPI_COMM_WORLD); */
   /* exit(1); */




  //localbsz = pc_data->localbsz;// = b_diag_mat->bsz;// changed

   //output_intvectorpa("localbsz",localbsz,0, param->n+1);


  /* Perform local ILU-type factorization */
  parms_PCILU(self, param, diag_mat, &op);

  pc_data->op = op;

  MPI_Comm_dup(A->is->comm, &pc_data->comm);

  /* get Krylov  subspace size */
  pc_data->im = param->ipar[4];
  //printf("pc_data->im = %d\n",pc_data->im);
  if (pc_data->im == 0) {
    pc_data->im = 5;
  }

  /* maximum inner iteration counts */
  pc_data->maxits = param->ipar[5];
  //printf("pc_data->maxits = %d\n",pc_data->maxits);
  if (pc_data->maxits == 0) {
    pc_data->maxits = 5;
  }

  /* 
     malloc memory for krylov subspace basis vv and Hessenberg matrix
  */
  PARMS_NEWARRAY(pc_data->vv, pc_data->im+1);

  /* Set the size of the matrix */
  pc_data->n = localbsz[param->n];//pc_data->n = param->n;//block version ??
  // printf("pc_data->n = %d\n",pc_data->n);

  /* Set the beginning location of the Schur complement */
  pc_data->schur_start = parms_OperatorGetSchurPos(op);//block version??

  //printf("pc_data->schur_start = %d\n",pc_data->schur_start);
/*   pc_data->schur_start = A->is->schur_start; */

/* initialise the localbsz, in the block version code, we set it to diag_mat->bsz*/
//Amat = (parms_bvcsr)mat;

//printf("pc_data->n = %d, pc_data->schur_start = %d \n",pc_data->n, pc_data->schur_start);


//printf("localbsz[pc_data->n] = %d, localbsz[pc_data->schur_start] = %d \n",localbsz[pc_data->n], localbsz[pc_data->schur_start]);
//printf("localbsz[1653] = %d, localbsz[980] = %d \n",localbsz[1653], localbsz[980]);

  /* The size of local Schur complement */
  //nrow = localbsz[pc_data->n] - pc_data->schur_start;//nrow = localbsz[pc_data->n] - localbsz[pc_data->schur_start];//nrow = pc_data->n - pc_data->schur_start;//changed
  nrow = pc_data->n - pc_data->schur_start;
  //printf(" nrow = %d \n",nrow);
 /* MPI_Barrier(MPI_COMM_WORLD); */
 /*  exit(1); */
  pc_data->nrow = nrow;



  /* Allocate memories for auxiliary arrays. */
  if(nrow) {
    for(i = 0; i < pc_data->im+1; i++) {
      PARMS_NEWARRAY(pc_data->vv[i], nrow);
    }
    PARMS_NEWARRAY(pc_data->z2, nrow);
    PARMS_NEWARRAY(pc_data->wk, nrow);
  }
  else {
    pc_data->z2 = NULL;
    pc_data->wk = NULL;
    pc_data->vv[0] = NULL;
  }

  PARMS_NEWARRAY(pc_data->hh, pc_data->im);
  for(i = 0; i < pc_data->im; i++) {
    PARMS_NEWARRAY(pc_data->hh[i], pc_data->im+1);
  }
  
  PARMS_NEWARRAY(pc_data->c,    pc_data->im);
  PARMS_NEWARRAY(pc_data->s,    pc_data->im);
  PARMS_NEWARRAY(pc_data->rs,   pc_data->im+1);
  PARMS_NEWARRAY(pc_data->hcol, pc_data->im+1);

  PARMS_FREE(localbsz);

  return 0;
}


/** Apply the preconditioner to the vector y.
 *
 *  This preconditioner is actually the lsch_xx preconditioners in old
 *  version of pARMS. 
 *
 *  \param self A preconditioner object.
 *  \param y    A right-hand-side vector.
 *  \param x    Solution vector.
 *  \return 0 on success.
 */
static int pc_schur_b_apply(parms_PC self, FLOAT *y, FLOAT *x)
{
  /*--------------------------------------------------------------------
    APPROXIMATE LU-SCHUR LEFT PRECONDITONER
    *--------------------------------------------------------------------
    Schur complement left preconditioner.  This is a preconditioner for 
    the global linear system which is based on solving approximately the
    Schur  complement system. More  precisely, an  approximation to the
    local Schur complement  is obtained (implicitly) in  the form of an
    LU factorization  from the LU  factorization  L_i U_i of  the local
    matrix A_i. Solving with this approximation amounts to simply doing
    the forward and backward solves with the bottom part of L_i and U_i
    only (corresponding to the interface variables).  This is done using
    a special version of the subroutine lusol0 called lusol0_p. Then it
    is possible to  solve  for an  approximate  Schur complement system
    using GMRES  on  the  global  approximate Schur  complement  system
    (which is preconditionied  by  the diagonal  blocks  represented by
    these restricted LU matrices).
    --------------------------------------------------------------------
    Coded by Y. Saad - Aug. 1997 - updated Nov. 14, 1997.
    C version coded by Zhongze Li, Jan. 2001, updated Sept, 17th, 2001
    Revised by Zhongze Li, June 1st, 2006.
    --------------------------------------------------------------------
  */
  schur_data     pc_data;
  parms_Operator op;
  parms_Map      is;
  parms_Mat      A;
  parms_FactParam param;
  MPI_Comm       comm;
  int schur_start,nrow, im,i,jj,i1,k,k1,its,j,ii,maxits,incx=1,ierr;
  int if_in_continue,if_out_continue;
  int ninf_len;//for block version length of ninf
  FLOAT *z2, *wk;
  FLOAT **vv, **hh, *s, *rs, *hcol, t1, alpha;
  REAL *c;
  REAL  eps,eps1,t,ro,tloc;  
  
#if defined(DBL_CMPLX)
   FLOAT rot;
#else
    REAL gam;    
#endif    
  static int iters = 0;

  /* retrieve the specific data structure for Schur based preconditioners  */ 
  pc_data = (schur_data)self->data;

  /* get the matrix A */
  A = self->A;

/* // NEW */
/*   /\* Get diagonal part of the matrix *\/ */
/*   parms_dvcsr data; */
/*   parms_bvcsr b_diag_mat; */
/*   int *bsz; */
/*   data = (parms_dvcsr)A->data; */
/*   b_diag_mat = data->b_diag_mat; */
/*   bsz = b_diag_mat->bsz; */
/* //END NEW */

// NEW
  /* Get diagonal part of the matrix */
  void *diag_mat;
  parms_bvcsr b_diag_mat;
  int *bsz;
  A->isin_b_schur = true;
  parms_MatGetDiag(A, &diag_mat);
  //A->isin_b_schur = false;
  //parms_MatGetDiag(A, &diag_mat);
  b_diag_mat = (parms_bvcsr)diag_mat;
  bsz = b_diag_mat->bsz;
//END NEW


  is = A->is;

//printf("In schur_apply A = %d\n", A );getchar();
//outputcsmatpa(A->aux_data,"aux_data",1);
//output_dblvectorpa("y_in_schur_apply",y,0, pc_data->nrow);//getchar();
  /* get the dimension of the local matrix */
  /* n = pc_data->n; */

  /* get the beginning location of the Schur complement */
  //schur_start = pc_data->localbsz[pc_data->schur_start];//schur_start = pc_data->schur_start;//changed 
  schur_start = pc_data->schur_start;

  /* get the size of the Schur complement */
  nrow = pc_data->nrow;

  /* get krylov space size */
  im = pc_data->im;

  /* get maximal iteration number */
  maxits = pc_data->maxits;

  /* get tolerance */
  param = self->param;
  eps = param->pgfpar[0];
  eps1 = eps;

  /* get communicator */
  comm = pc_data->comm;

  /* get working arrays */
  vv   = pc_data->vv;
  hh   = pc_data->hh;
  c    = pc_data->c;
  s    = pc_data->s;
  rs   = pc_data->rs;
  hcol = pc_data->hcol;
  z2   = pc_data->z2;
  wk   = pc_data->wk;

  /* retrieve the operator */
  op = pc_data->op;
  /* compute the right-hand side of Schur Complement System
     it turns out that  this is the bottom part of inv(A_i)*rhs
  */

  parms_OperatorLsol(op, y, x);//vbarms Lsol is untested

  GCOPY(nrow, &x[schur_start], incx, wk, incx);
  parms_OperatorInvS(op, &x[schur_start], vv[0]);//vbarms invs is untested 

  for (i = 0; i < nrow; i++) {
    x[schur_start+i] = 0.0;
  }


  /* solution loop by gmres. */
  if_out_continue = 1;
  its = 0;

  while(if_out_continue) {
#if defined(DBL_CMPLX)  
    t1 = GDOTC(nrow, vv[0], incx, vv[0], incx);
    t = creal(t1);
#else
    t = GDOT(nrow, vv[0], incx, vv[0], incx);
#endif
    MPI_Allreduce(&t, &ro, 1, MPI_DOUBLE, MPI_SUM, comm);
    ro = sqrt(ro);
    if(fabs(ro - ZERO) <= EPSILON) { 
      ierr = -1;
      break;
    }

    t1 = 1.0/ro;

    GSCAL(nrow, t1, vv[0], incx);

    if(its == 0) eps1 = eps*ro;
    /* initialize 1-st term of rhs of hessenberg system. */
#if defined(DBL_CMPLX)
    rs[0] = ro + 0.0*I;
#else
    rs[0] = ro;
#endif
    i = -1;
    if_in_continue = 1;
    while(if_in_continue) {
      ++i;
      ++its;
      i1 = i + 1;
  
      /* 
	 matrix-vector product
	 tmp = (0,y)^t: first fill tmp with zero components from 1 to n
	 then copy rhs into "y" part of tmp
      */
      /* printf("schur_start = %d\n", schur_start); */
     /* printf("bsz[is->nint] = %d\n", bsz[is->nint]); */
     /* printf("is->nint = %d\n", is->nint); */
     /* printf("is->ninf = %d\n", is->ninf); */
      parms_MatVecOffDiag(A, vv[i], z2, schur_start);//change schur_start

      //output_intvectorpa("b_diag_mat_bsz_schur",b_diag_mat->bsz,0, param->n);
      //MPI_Barrier(MPI_COMM_WORLD);
      //exit(1);

      parms_OperatorInvS(op, z2, z2);

      /* result = input + last part of z2 */
      for(j = 0; j < nrow; j++) {
	vv[i1][j] = vv[i][j] + z2[j];
      }

      /* modified gram - schmidt .. */
#if defined(DBL_CMPLX)      
      for(j = 0; j <= i; j++) {
	hcol[j] = GDOTC(nrow, vv[j], incx, vv[i1], incx);
      }
      MPI_Allreduce(hcol, hh[i], i+1, MPI_CMPLX, MPI_CMPLX_SUM, comm);      
#else
      for(j = 0; j <= i; j++) {
	hcol[j] = GDOT(nrow, vv[j], incx, vv[i1], incx);
      }
      MPI_Allreduce(hcol, hh[i], i+1, MPI_DOUBLE, MPI_SUM, comm);      
#endif

      for(j = 0; j <= i; j++) {
	alpha = -hh[i][j];
	GAXPY(nrow, alpha, vv[j], incx, vv[i1], incx);
      }
#if defined(DBL_CMPLX)
      t1 = GDOTC(nrow, vv[i1], incx, vv[i1], incx);
      tloc = creal(t1);
#else
      tloc = GDOT(nrow, vv[i1], incx, vv[i1], incx);
#endif
      MPI_Allreduce(&tloc, &t, 1, MPI_DOUBLE, MPI_SUM, comm);
      t = sqrt(t);
#if defined(DBL_CMPLX)      
      hh[i][i1] = t + 0.0*I;
#else
      hh[i][i1] = t;
#endif
      if(fabs(t - ZERO) > EPSILON) {
	t1 = 1.0/t;
	GSCAL(nrow, t1, vv[i1], incx);
      }
      /* 
	 done with modified gram Schmidt and Arnoldi step..
	 now update factorization of hh 
      */
      if(i != 0) {
	/* perform previous transformations on i-th column of h */
#if defined(DBL_CMPLX)	
	for(k = 1; k <= i; k++) {
	  k1 = k-1;
	  t1 = hh[i][k1];
	  hh[i][k1] = c[k1]*t1 + s[k1]*hh[i][k];
	  hh[i][k] = -conj(s[k1])*t1 + c[k1]*hh[i][k];
	}
#else
	for(k = 1; k <= i; k++) {
	  k1 = k-1;
	  t1 = hh[i][k1];
	  hh[i][k1] = c[k1]*t1 + s[k1]*hh[i][k];
	  hh[i][k] = -s[k1]*t1 + c[k1]*hh[i][k];
	}
#endif
      }
      
#if defined(DBL_CMPLX)
/*-----------get next plane rotation------------ */
      zclartg(hh[i][i], hh[i][i1], &c[i], &s[i], &rot);
      rs[i1] = -conj(s[i])*rs[i];
      rs[i] = c[i]*rs[i];      
      hh[i][i] = rot;
      ro = cabs(rs[i1]);
#else
      gam = sqrt(hh[i][i]*hh[i][i] + hh[i][i1]*hh[i][i1]);
      /* 
	 if gamma is zero then any small value will do ...
	 will affect only residual estimate 
      */
      if(fabs(gam-ZERO) < EPSILON) gam = EPSMAC;
      /* get next plane rotation */
      c[i] = hh[i][i] / gam;
      s[i] = hh[i][i1] / gam;
      rs[i1] = -s[i]*rs[i];
      rs[i] = c[i]*rs[i];
 
      /* determine residual norm and test for convergence */
      hh[i][i] = c[i]*hh[i][i] + s[i]*hh[i][i1];
      ro = fabs(rs[i1]);
#endif

      if(i+1 >= im || ro <= eps1 || its >= maxits)
	if_in_continue = 0;
    }

    /* now compute solution. first solve upper triangular system. */
    for(ii = i; ii >= 0; --ii) {
      t1 = rs[ii];
      for(j = ii+1; j <= i; j++) {
	t1 = t1 - hh[j][ii]*rs[j];
      }
      rs[ii] = t1 / hh[ii][ii];
    }

    /* form linear combination of v(*,i)'s to update solution */
    for(j = 0; j < i+1; j++) {
      GAXPY(nrow, rs[j], vv[j], incx, &x[schur_start], incx);
    }
    /* restart outer loop when necessary */
    if(ro <= eps1) {
      ierr = 0;
      if_out_continue = 0;
    }
    else if(its >= maxits) {
      ierr = 1;
      if_out_continue = 0;
    }
    /* else compute residual vector (from the v's) and continue.. */
    else {
#if defined(DBL_CMPLX)    
      for(j = 0; j <= i; j++) {
	jj= i1-j+1;
	rs[jj-1] = -conj(s[jj-1])*rs[jj];
	rs[jj] = c[jj-1]*rs[jj];
      }
#else
      for(j = 0; j <= i; j++) {
	jj= i1-j+1;
	rs[jj-1] = -s[jj-1]*rs[jj];
	rs[jj] = c[jj-1]*rs[jj];
      }
#endif   
      for(j = 0; j <= i1; j++) {
	t1 = rs[j];
	if(j == 0) t1 = t1-1.0;
	GAXPY(nrow, t1, vv[j], incx, vv[0], incx);
      }
    }
  }
  /* 
     post-processing to compute interior variables..
     compute RHS for last system. similar to Schur iteration
     first get a vector of the form (0,y)^T
  */
  parms_MatVecOffDiag(A, &x[schur_start], z2, schur_start); 

  GCOPY(nrow, wk, incx, &x[schur_start], incx); 

  /* //print to debug  */
  /* int myid; */
  /* MPI_Comm_rank(MPI_COMM_WORLD, &myid); */
  /* printf("bsz[is->ninf] = %d, pid = %d\n", bsz[is->ninf],myid);  */
  /* printf("bsz[is->nint] = %d, pid = %d\n", bsz[is->nint],myid);  */
  /* printf("schur_start = %d, pid = %d\n", schur_start,myid);  */
  /* //debug end  */
  ninf_len  = is->llsize-bsz[is->nint];
  //for (i = 0; i < bsz[is->ninf]; i++) {
  for (i = 0; i < ninf_len; i++) {
    x[i+bsz[is->nint]] -= z2[i+bsz[is->nint]-schur_start]; //fixed
  }
  /* solve the Schur complement system */
  parms_OperatorInvS(op, &x[schur_start], &x[schur_start]);
  /* backward sweep */
  parms_OperatorAscend(op, x, x);

  iters += its;
  pc_data->in_iters = iters;

  return ierr;
}





/** Create a Schur-complement based preconditioner.
 *
 *  \param self A preconditioner object.
 *  \return 0 on success.
 */
int parms_PCCreate_b_Schur(parms_PC self)
{
  schur_data data;

  PARMS_NEW(data);
  self->data = data;
  self->ops->pc_free     = pc_schur_free;
  self->ops->pc_view     = pc_schur_view;
  self->ops->apply    = pc_schur_b_apply;
  self->ops->setup    = pc_schur_b_setup;
  self->ops->getratio = pc_schur_getratio;
  return 0;
}

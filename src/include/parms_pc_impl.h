#ifndef _PARMS_PC_IMPL_H_
#define _PARMS_PC_IMPL_H_

#include "parms_pc.h"
#include "parms_vec.h"
#include "parms_mat_impl.h"
#include "parms_viewer.h"

typedef struct parms_PC_ops {
  int (*apply)(parms_PC self, FLOAT *y, FLOAT *x);
  int (*setup)(parms_PC self);
  int (*getratio)(parms_PC self, double *ratio);
  int (*pc_free)(parms_PC *self);
  int (*pc_view)(parms_PC self, parms_Viewer v); 
} *parms_PC_ops;

extern char *pcname[];
extern char *pciluname[];

struct parms_PC_ {

  int ref;
  parms_PC_ops     ops;
  void             *data;
  parms_Mat        A;
  BOOL             istypeset;
  BOOL             isiluset;
  BOOL             issetup;
  BOOL             isopset;
  BOOL             isperm;
  BOOL             islocalgc;//new, for judging if we use local graph compression or the global one.
  int              *perm;
  int              *iperm;
  parms_FactParam  param;
  PCTYPE           pctype;
  PCILUTYPE        pcilutype;
};

/*-----external function protos--------*/
extern int parms_PCCreate_BJ(parms_PC self);
extern int parms_PCCreate_Schur(parms_PC self);
extern int parms_PCCreate_RAS(parms_PC self);
//---------------------------------------------------------------------------------------------------dividing line----------------------------------------------------
extern int parms_PCCreate_b_BJ(parms_PC self);
extern int parms_PCCreate_b_Schur(parms_PC self);
extern int parms_PCCreate_b_RAS(parms_PC self);
/*----------End Protos------------------*/

#endif 

#include <assert.h>
#include <string.h>

#include <sqlite3ext.h>
extern const sqlite3_api_routines *sqlite3_api;

#include "chemicalite.h"
#include "rdkit_adapter.h"
#include "utils.h"
#include "bitstring.h"

/*
** fetch Bfp from blob argument
*/
int fetch_bfp_arg(sqlite3_value* arg, Bfp **ppBfp)
{
  int rc = SQLITE_MISMATCH;
  /* Check that value is a blob */
  if (sqlite3_value_type(arg) == SQLITE_BLOB) {
    int sz = sqlite3_value_bytes(arg);
    rc = blob_to_bfp(sqlite3_value_blob(arg), sz, ppBfp);
  }
  return rc;
}

/*
** Mol -> Bfp conversion
*/
#define MOL_TO_BFP(func)						\
  static void func##_f(sqlite3_context* ctx,				\
		       int argc, sqlite3_value** argv)			\
  {									\
    assert(argc == 1);							\
    int rc = SQLITE_OK;							\
    Mol *pMol = 0;							\
    rc = fetch_mol_arg(argv[0], &pMol);					\
    if (rc != SQLITE_OK) goto func##_f_end;				\
									\
    Bfp * pBfp;								\
    rc = func(pMol, &pBfp);						\
    if (rc != SQLITE_OK) goto func##_f_free_mol;			\
									\
    u8 * pBlob = 0;                                                     \
    int sz = 0;								\
    rc = bfp_to_blob(pBfp, &pBlob, &sz);				\
									\
  func##_f_free_bfp: free_bfp(pBfp);					\
  func##_f_free_mol: free_mol(pMol);					\
  func##_f_end:								\
    if (rc == SQLITE_OK) {						\
      sqlite3_result_blob(ctx, pBlob, sz, sqlite3_free);		\
    }									\
    else {								\
      sqlite3_result_error_code(ctx, rc);				\
    }									\
  }
    
MOL_TO_BFP(mol_layered_bfp)
MOL_TO_BFP(mol_rdkit_bfp)
MOL_TO_BFP(mol_atom_pairs_bfp)
MOL_TO_BFP(mol_topological_torsion_bfp)
MOL_TO_BFP(mol_maccs_bfp)

#define MOL_TO_MORGAN_BFP(func)						\
  static void func##_f(sqlite3_context* ctx,				\
		       int argc, sqlite3_value** argv)			\
  {									\
    assert(argc == 2);							\
    int rc = SQLITE_OK;							\
									\
    Mol *pMol = 0;							\
    rc = fetch_mol_arg(argv[0], &pMol);					\
    if (rc != SQLITE_OK) goto func##_f_end;				\
									\
    int radius = sqlite3_value_int(argv[1]);				\
									\
    Bfp * pBfp;								\
    rc = func(pMol, radius, &pBfp);					\
    if (rc != SQLITE_OK) goto func##_f_free_mol;			\
									\
    u8 * pBlob = 0;                                                     \
    int sz = 0;								\
    rc = bfp_to_blob(pBfp, &pBlob, &sz);				\
									\
  func##_f_free_bfp: free_bfp(pBfp);					\
  func##_f_free_mol: free_mol(pMol);					\
  func##_f_end:								\
    if (rc == SQLITE_OK) {						\
      sqlite3_result_blob(ctx, pBlob, sz, sqlite3_free);		\
    }									\
    else {								\
      sqlite3_result_error_code(ctx, rc);				\
    }									\
  }

MOL_TO_MORGAN_BFP(mol_morgan_bfp)
MOL_TO_MORGAN_BFP(mol_feat_morgan_bfp)

/*
** Slightly different, but still a bfp
*/
MOL_TO_BFP(mol_bfp_signature)

/*
** bitstring similarity
*/

#define COMPARE_BITSTRINGS(sim)					\
  static void bfp_##sim##_f(sqlite3_context* ctx,			\
			     int argc, sqlite3_value** argv)		\
  {									\
    assert(argc == 2);							\
    double similarity = 0.;						\
    int rc = SQLITE_OK;							\
									\
    Bfp *p1 = 0;							\
    Bfp *p2 = 0;							\
									\
    rc = fetch_bfp_arg(argv[0], &p1);					\
    if (rc != SQLITE_OK) goto sim##_f_end;				\
									\
    rc = fetch_bfp_arg(argv[1], &p2);					\
    if (rc != SQLITE_OK) goto sim##_f_free_bfp1;			\
									\
    if (bfp_length(p1) != bfp_length(p2)) {				\
      rc = SQLITE_MISMATCH;						\
    }									\
    else {								\
      similarity = bfp_##sim(p1, p2);					\
    }									\
      									\
  sim##_f_free_bfp2: free_bfp(p2);					\
  sim##_f_free_bfp1: free_bfp(p1);					\
  sim##_f_end:								\
    if (rc == SQLITE_OK) {						\
      sqlite3_result_double(ctx, similarity);				\
    }									\
    else {								\
      sqlite3_result_error_code(ctx, rc);				\
    }									\
  }

/*
** bitstring similarity
*/
COMPARE_BITSTRINGS(tanimoto)
COMPARE_BITSTRINGS(dice)

/*
** build a simple bitstring (mostly for testing)
*/
static void bfp_dummy_f(sqlite3_context* ctx,
			int argc, sqlite3_value** argv)
{
  assert(argc == 2);
  int rc = SQLITE_OK;
  int len, value;

  u8 * pBlob = 0;

  /* Check that value is a blob */
  if (sqlite3_value_type(argv[0]) != SQLITE_INTEGER) {
    rc = SQLITE_MISMATCH;
  }
  else if (sqlite3_value_type(argv[1]) != SQLITE_INTEGER) {
    rc = SQLITE_MISMATCH;
  }
  else {

    len = sqlite3_value_int(argv[0]);
    if (len <= 0) { len = 1; }
    if (len > MAX_BITSTRING_SIZE) { len = MAX_BITSTRING_SIZE; }

    value = sqlite3_value_int(argv[1]);
    if (value < 0) { value = 0; }
    if (value > 255) { value = 255; }

    pBlob = (u8 *)sqlite3_malloc(len);
    if (!pBlob) {
      rc = SQLITE_NOMEM;
    }
    else {
      u8 *p = pBlob; 
      int ii;
      for (ii = 0; ii < len; ++ii) {
	*p++ = value;
      }
    }

  }

  if (rc == SQLITE_OK) {
    sqlite3_result_blob(ctx, pBlob, len, sqlite3_free);
  }	
  else {
    sqlite3_result_error_code(ctx, rc);
  }

}

/*
** bitstring length and weight
*/

static void bfp_length_f(sqlite3_context* ctx,
			 int argc, sqlite3_value** argv)
{
  assert(argc == 1);
  int rc = SQLITE_OK;
  
  Bfp *pBfp = 0;
  rc = fetch_bfp_arg(argv[0], &pBfp);

  if (rc == SQLITE_OK) {
    int length = bfp_length(pBfp);
    free_bfp(pBfp);
    sqlite3_result_int(ctx, length);    
  }
  else {
    sqlite3_result_error_code(ctx, rc);
  }
}

static void bfp_weight_f(sqlite3_context* ctx,
			 int argc, sqlite3_value** argv)
{
  assert(argc == 1);
  int rc = SQLITE_OK;
  
  Bfp *pBfp = 0;
  rc = fetch_bfp_arg(argv[0], &pBfp);

  if (rc == SQLITE_OK) {
    int weight = bfp_weight(pBfp);
    free_bfp(pBfp);
    sqlite3_result_int(ctx, weight);    
  }
  else {
    sqlite3_result_error_code(ctx, rc);
  }
}

int chemicalite_init_bitstring(sqlite3 *db)
{
  int rc = SQLITE_OK;

  CREATE_SQLITE_BINARY_FUNCTION(bfp_tanimoto, rc);
  CREATE_SQLITE_BINARY_FUNCTION(bfp_dice, rc);

  CREATE_SQLITE_UNARY_FUNCTION(bfp_length, rc);
  CREATE_SQLITE_UNARY_FUNCTION(bfp_weight, rc);

  CREATE_SQLITE_UNARY_FUNCTION(mol_layered_bfp, rc);
  CREATE_SQLITE_UNARY_FUNCTION(mol_rdkit_bfp, rc);
  CREATE_SQLITE_UNARY_FUNCTION(mol_atom_pairs_bfp, rc);
  CREATE_SQLITE_UNARY_FUNCTION(mol_topological_torsion_bfp, rc);
  CREATE_SQLITE_UNARY_FUNCTION(mol_maccs_bfp, rc);

  CREATE_SQLITE_BINARY_FUNCTION(mol_morgan_bfp, rc);
  CREATE_SQLITE_BINARY_FUNCTION(mol_feat_morgan_bfp, rc);

  CREATE_SQLITE_UNARY_FUNCTION(mol_bfp_signature, rc);

  CREATE_SQLITE_BINARY_FUNCTION(bfp_dummy, rc);

  return rc;
}

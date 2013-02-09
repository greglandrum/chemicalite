#include <cassert>
#include <cstring>
#include <string>

#include <sqlite3ext.h>
extern const sqlite3_api_routines *sqlite3_api;

#include <GraphMol/RDKitBase.h>
#include <GraphMol/MolPickler.h>
#include <GraphMol/Descriptors/MolDescriptors.h>
#include <GraphMol/SmilesParse/SmilesParse.h>
#include <GraphMol/SmilesParse/SmartsWrite.h>
#include <GraphMol/SmilesParse/SmilesWrite.h>
#include <GraphMol/Substruct/SubstructMatch.h>
#include <GraphMol/Fingerprints/Fingerprints.h>
#include <DataStructs/ExplicitBitVect.h>
#include <DataStructs/BitOps.h>

#include "chemicalite.h"
#include "rdkit_adapter.h"

struct Mol : RDKit::ROMol {
  // Mol() : RDKit::ROMol() {}
  // Mol(const Mol & other) : RDKit::ROMol(other) {}
  // Mol(const RDKit::ROMol & other) : RDKit::ROMol(other) {}
  Mol(const std::string & pickle) : RDKit::ROMol(pickle) {}
};

void free_mol(Mol *pMol)
{
  delete static_cast<RDKit::ROMol *>(pMol);
}

struct BitString : ExplicitBitVect {
  BitString(const char * d, const unsigned int n) : ExplicitBitVect(d, n) {}
};

void free_bitstring(BitString *pBits)
{
  delete static_cast<ExplicitBitVect *>(pBits);
}

namespace {
  const unsigned int SSS_FP_SIZE         = 8*MOL_SIGNATURE_SIZE;
  const unsigned int LAYERED_FP_SIZE     = 1024;
  const unsigned int MORGAN_FP_SIZE      = 1024;
  const unsigned int HASHED_PAIR_FP_SIZE = 2048;
}

// SMILES/SMARTS <-> Molecule ////////////////////////////////////////////////

int txt_to_mol(const char * txt, int as_smarts, Mol **ppMol)
{
  assert(txt);
  int rc = SQLITE_OK;

  *ppMol = 0;

  try {
    std::string data(txt);
    RDKit::ROMol *pROMol = as_smarts ?
      RDKit::SmartsToMol(data) : RDKit::SmilesToMol(data);
      *ppMol = static_cast<Mol *>(pROMol);
  } 
  catch (...) {
    // problem generating molecule from smiles
    rc = SQLITE_ERROR;
  }
  if (!*ppMol) {
    // parse error
    rc = SQLITE_ERROR;
  }

  return rc;
}

int mol_to_txt(Mol *pMol, int as_smarts, char **pTxt)
{
  assert(pMol);
  *pTxt = 0;
  int rc = SQLITE_OK;

  std::string txt;
  try {
    txt.assign( as_smarts ? 
		RDKit::MolToSmarts(*pMol, false) : 
		RDKit::MolToSmiles(*pMol, true) );
  } 
  catch (...) {
    // unknown exception
    rc = SQLITE_ERROR;
  }       

  if (rc == SQLITE_OK) {
    *pTxt = sqlite3_mprintf("%s", txt.c_str());
    if (!(*pTxt)) {
      rc = SQLITE_NOMEM;
    }
  }

  return rc;               
}

// Blob <-> Molecule /////////////////////////////////////////////////////////

int blob_to_mol(u8 *pBlob, int len, Mol **ppMol)
{
  assert(pBlob);
  *ppMol = 0;

  int rc = SQLITE_OK;

  std::string blob;
  try {
    blob.assign((const char *)pBlob, len);
    *ppMol = new Mol(blob);
  } 
  catch (...) {
    // problem generating molecule from blob data
    rc = SQLITE_ERROR;
  }
  
  if (!(*ppMol)) {
    // blob data could not be parsed
    rc = SQLITE_ERROR;
  }

  return rc;
}

int mol_to_blob(Mol *pMol, u8 **ppBlob, int *pLen)
{
  assert(pMol);

  *ppBlob = 0;
  *pLen = 0;

  int rc = SQLITE_OK;
  std::string blob;
  try {
    RDKit::MolPickler::pickleMol(*pMol, blob);
  } 
  catch (...) {
    // unknown exception
    rc = SQLITE_ERROR;
  }       

  if (rc == SQLITE_OK) {
    *ppBlob = (u8 *)sqlite3_malloc(blob.size());
    if (*ppBlob) {
      memcpy(*ppBlob, blob.data(), blob.size());
      *pLen = blob.size();
    }
    else {
      rc = SQLITE_NOMEM;
    }
  }

  return rc;
}

// Blob <-> SMILES/SMARTS ////////////////////////////////////////////////////

int txt_to_blob(const char * txt, int as_smarts, u8 **pBlob, int *pLen)
{
  Mol * pMol = 0;
  int rc = txt_to_mol(txt, as_smarts, &pMol);
  if (rc == SQLITE_OK) {
    rc = mol_to_blob(pMol, pBlob, pLen);
    free_mol(pMol);
  }
  return rc;
}

int blob_to_txt(u8 *blob, int len, int as_smarts, char **pTxt)
{
  Mol * pMol = 0;
  int rc = blob_to_mol(blob, len, &pMol);
  if (rc == SQLITE_OK) {
    rc = mol_to_txt(pMol, as_smarts, pTxt);
    free_mol(pMol);
  }
  return rc;
}

// Molecule -> signature /////////////////////////////////////////////////////

int mol_signature(Mol *pMol, u8 **ppSign, int *pLen)
{
  assert(pMol);

  *ppSign = 0;
  *pLen = 0;

  int rc = SQLITE_OK;
  BitString *pBits = 0;

  try {
    ExplicitBitVect *bv 
      = RDKit::LayeredFingerprintMol(*pMol, RDKit::substructLayers, 1, 6, 
				     SSS_FP_SIZE);
    if (bv) {
      rc = bitstring_to_blob(static_cast<BitString *>(bv), ppSign, pLen);
      delete bv;
    }
    else {
      rc = SQLITE_ERROR;
    }
  } 
  catch (...) {
    // unknown exception
    rc = SQLITE_ERROR;
  }
        
  return rc;
}

// Molecules comparison //////////////////////////////////////////////////////

int mol_is_substruct(Mol *p1, Mol *p2)
{
  assert(p1 && p2);
  RDKit::MatchVectType matchVect;
  return RDKit::SubstructMatch(*p1, *p2, matchVect) ? 1 : 0; 
}

int mol_is_superstruct(Mol *p1, Mol *p2)
{
  return mol_is_substruct(p2, p1);
}

int mol_cmp(Mol *p1, Mol *p2)
{
  assert(p1 && p2);
  
  int res = p1->getNumAtoms() - p2->getNumAtoms();
  if (res) {return (res > 0) ? 1 : -1;}

  res = p1->getNumBonds() - p2->getNumBonds();
  if (res) {return (res > 0) ? 1 : -1;}

  res = int(RDKit::Descriptors::calcAMW(*p1, false) -
	    RDKit::Descriptors::calcAMW(*p2, false) + .5);
  if (res) {return (res > 0) ? 1 : -1;}

  res = p1->getRingInfo()->numRings() - p2->getRingInfo()->numRings();
  if (res) {return (res > 0) ? 1 : -1;}

  // FIXME if not the same result is -1 also if the args are swapped
  return mol_is_substruct(p1, p2) ? 0 : -1; 
}

// Molecular descriptors /////////////////////////////////////////////////////

#define MOL_DESCRIPTOR(name, func, type)		\
  type mol_##name(Mol *pMol)				\
  {							\
    assert(pMol);					\
    return func(*pMol);					\
  }

MOL_DESCRIPTOR(mw, RDKit::Descriptors::calcAMW, double)
MOL_DESCRIPTOR(tpsa, RDKit::Descriptors::calcTPSA, double)
MOL_DESCRIPTOR(hba, RDKit::Descriptors::calcLipinskiHBA, int)
MOL_DESCRIPTOR(hbd, RDKit::Descriptors::calcLipinskiHBD, int)
MOL_DESCRIPTOR(num_rotatable_bnds, RDKit::Descriptors::calcNumRotatableBonds, 
	       int)
MOL_DESCRIPTOR(num_hetatms, RDKit::Descriptors::calcNumHeteroatoms, int)
MOL_DESCRIPTOR(num_rings, RDKit::Descriptors::calcNumRings, int)
MOL_DESCRIPTOR(chi0v,RDKit::Descriptors::calcChi0v,double)
MOL_DESCRIPTOR(chi1v,RDKit::Descriptors::calcChi1v,double)
MOL_DESCRIPTOR(chi2v,RDKit::Descriptors::calcChi2v,double)
MOL_DESCRIPTOR(chi3v,RDKit::Descriptors::calcChi3v,double)
MOL_DESCRIPTOR(chi4v,RDKit::Descriptors::calcChi4v,double)
MOL_DESCRIPTOR(chi0n,RDKit::Descriptors::calcChi0n,double)
MOL_DESCRIPTOR(chi1n,RDKit::Descriptors::calcChi1n,double)
MOL_DESCRIPTOR(chi2n,RDKit::Descriptors::calcChi2n,double)
MOL_DESCRIPTOR(chi3n,RDKit::Descriptors::calcChi3n,double)
MOL_DESCRIPTOR(chi4n,RDKit::Descriptors::calcChi4n,double)
MOL_DESCRIPTOR(kappa1,RDKit::Descriptors::calcKappa1,double)
MOL_DESCRIPTOR(kappa2,RDKit::Descriptors::calcKappa2,double)
MOL_DESCRIPTOR(kappa3,RDKit::Descriptors::calcKappa3,double)
  
double mol_logp(Mol *pMol) 
{
  assert(pMol);
  double logp, mr;
  RDKit::Descriptors::calcCrippenDescriptors(*pMol, logp, mr);
  return logp;
}

int mol_num_atms(Mol *pMol) 
{
  assert(pMol);
  return pMol->getNumAtoms(false);
}

int mol_num_hvyatms(Mol *pMol) 
{
  assert(pMol);
  return pMol->getNumAtoms(true);
}


// BitString <-> Blob ///////////////////////////////////////////////////////

int bitstring_to_blob(BitString *pBits, u8 **ppBlob, int *pLen)
{
  assert(pBits);
  *ppBlob = 0;
  *pLen = 0;

  int rc = SQLITE_OK;

  int num_bits = pBits->getNumBits();
  int num_bytes = num_bits/8;
  if (num_bits % 8) ++num_bytes;

  *ppBlob = (u8 *)sqlite3_malloc(num_bytes);

  u8 *s = *ppBlob;

  if (!s) {
    rc = SQLITE_NOMEM;
  }
  else {
    memset(s, 0, num_bytes);
    *pLen = num_bytes;
    for (int i = 0; i < num_bits; ++i) {
      if (!pBits->getBit(i)) { continue; }
      s[ i/8 ]  |= 1 << (i % 8);
    }
  }

  return rc;
}

int blob_to_bitstring(u8 *pBlob, int len, BitString **ppBits)
{
  assert(pBlob);
  int rc = SQLITE_OK;
  *ppBits = 0;
        
  try {
    if (!( *ppBits = new BitString((const char *)pBlob, len) )) {
      rc = SQLITE_NOMEM;
    }
  } catch (...) {
    // Unknown exception
    rc = SQLITE_ERROR;
  }

  return rc;       
}

// BitString <-> Blob ///////////////////////////////////////////////////////

int bitstring_tanimoto(BitString *pBits1, BitString *pBits2, double *pSim)
{
  int rc = SQLITE_OK;
  *pSim = 0.0;

  // Nsame / (Na + Nb - Nsame)
        
  try {
    *pSim = TanimotoSimilarity(*static_cast<ExplicitBitVect *>(pBits1), 
			       *static_cast<ExplicitBitVect *>(pBits2));
  } 
  catch (ValueErrorException& e) {
    // TODO investigate possible causes for this exc
    rc = SQLITE_ERROR;
  } 
  catch (...) {
    // unknown exception
    rc = SQLITE_ERROR;
  }

  return rc;
}

int bitstring_dice(BitString *pBits1, BitString *pBits2, double *pSim)
{
  int rc = SQLITE_OK;
  *pSim = 0.0;

  // 2 * Nsame / (Na + Nb)
        
  try {
    *pSim = DiceSimilarity(*static_cast<ExplicitBitVect *>(pBits1), 
			   *static_cast<ExplicitBitVect *>(pBits2));
  } 
  catch (ValueErrorException& e) {
    // TODO investigate possible causes for this exc
    rc = SQLITE_ERROR;
  } 
  catch (...) {
    // unknown exception
    rc = SQLITE_ERROR;
  }

  return rc;
}

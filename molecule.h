#ifndef CHEMICALITE_MOLECULE_INCLUDED
#define CHEMICALITE_MOLECULE_INCLUDED

int chemicalite_init_molecule(sqlite3 *db);
int fetch_mol_arg(sqlite3_value* arg, Mol **ppMol);

#endif

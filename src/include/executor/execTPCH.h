/*-------------------------------------------------------------------------
 *
 * execTPCH.h
 *	  header for generating TPC-H updates 
 *
 *
 * src/include/executor/execTPCH.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef EXECTPCH_H
#define EXECTPCH_H 

extern char *tables_with_update;
extern double bd_prob; 
extern enum tpch_delta_mode delta_mode;
extern double wrong_exp_delta;
extern double exp_delta; 

typedef enum tpch_delta_mode {
    TPCH_DEFAULT,
    TPCH_UNIFORM,
    TPCH_DECAY, 
    TPCH_BINOMIAL 
} tpch_delta_mode;

typedef struct TPCH_Delta
{
    int max_row; 
    int *delta_array; 
} TPCH_Delta; 

typedef struct TPCH_Update 
{
    int     numUpdates;
    int     *table_oid; 
    double  *table_amp_factor;
    bool    *delta_occur;
    bool    *table_complete;
    char    **update_tables;
    char    **update_commands;
    char    **delete_commands;
    int     numdelta;
    TPCH_Delta *tpch_delta; /* indexed by tables and then by number of deltas*/
    int *exist_mask;
} TPCH_Update; 

extern TPCH_Update *ExecInitTPCHUpdate(char *tablenames, bool wrong_prediction); 

extern TPCH_Update *BuildTPCHUpdate(char *tablenames);

extern int PopulateUpdate(TPCH_Update *update, int numdelta, bool wrong_prediction); 

extern int CheckTPCHUpdate(TPCH_Update *update, int oid, int delta_index);

extern void GenTPCHUpdate(TPCH_Update *update, int delta_index); 

extern TPCH_Update *DefaultTPCHUpdate(int numDelta); 

extern bool CheckTPCHDefaultUpdate(int oid);

extern char * GetTableName(TPCH_Update *update, int oid);

extern void ExtMarkDeltaOccur(TPCH_Update *update, int oid);

extern bool ExtAllExternalDeltaOccur(TPCH_Update *update);

extern int ExtCheckTPCHUpdate(TPCH_Update *update, int oid);

#endif 


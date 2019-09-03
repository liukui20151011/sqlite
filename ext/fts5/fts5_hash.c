/*
** 2014 August 11
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
*/



#include "fts5Int.h"

typedef struct Fts5HashEntry Fts5HashEntry;

/*
** This file contains the implementation of an in-memory hash table used
** to accumuluate "term -> doclist" content before it is flused to a level-0
** segment.
*/


struct Fts5Hash {
  int *pnByte;                    /* Pointer to bytes counter */
  int nEntry;                     /* Number of entries currently in hash */
  int nSlot;                      /* Size of aSlot[] array */
  Fts5HashEntry *pScan;           /* Current ordered scan item */
  Fts5HashEntry **aSlot;          /* Array of hash slots */
};

/*
** Each entry in the hash table is represented by an object of the 
** following type. Each object, its key (zKey[]) and its current data
** are stored in a single memory allocation. The position list data 
** immediately follows the key data in memory.
**
** The data that follows the key is in a similar, but not identical format
** to the doclist data stored in the database. It is:
**
**   * Rowid, as a varint
**   * Position list, without 0x00 terminator.
**   * Size of previous position list and rowid, as a 4 byte
**     big-endian integer.
**
** iRowidOff:
**   Offset of last rowid written to data area. Relative to first byte of
**   structure.
**
** nData:
**   Bytes of data written since iRowidOff.
*/
struct Fts5HashEntry {
  Fts5HashEntry *pHashNext;       /* Next hash entry with same hash-key */
  Fts5HashEntry *pScanNext;       /* Next entry in sorted order */
  
  int nAlloc;                     /* Total size of allocation */
  int iSzPoslist;                 /* Offset of space for 4-byte poslist size */
  int nData;                      /* Total bytes of data (incl. structure) */
  u8 bDel;                        /* Set delete-flag @ iSzPoslist */

  int iCol;                       /* Column of last value written */
  int iPos;                       /* Position of last value written */
  i64 iRowid;                     /* Rowid of last value written */
  char zKey[8];                   /* Nul-terminated entry key */
};

/*
** Size of Fts5HashEntry without the zKey[] array.
*/
#define FTS5_HASHENTRYSIZE (sizeof(Fts5HashEntry)-8)



/*
** Allocate a new hash table.
*/
int sqlite3Fts5HashNew(Fts5Hash **ppNew, int *pnByte){
  int rc = SQLITE_OK;
  Fts5Hash *pNew;

  *ppNew = pNew = (Fts5Hash*)sqlite3_malloc(sizeof(Fts5Hash));
  if( pNew==0 ){
    rc = SQLITE_NOMEM;
  }else{
    int nByte;
    memset(pNew, 0, sizeof(Fts5Hash));
    pNew->pnByte = pnByte;

    pNew->nSlot = 1024;
    nByte = sizeof(Fts5HashEntry*) * pNew->nSlot;
    pNew->aSlot = (Fts5HashEntry**)sqlite3_malloc(nByte);
    if( pNew->aSlot==0 ){
      sqlite3_free(pNew);
      *ppNew = 0;
      rc = SQLITE_NOMEM;
    }else{
      memset(pNew->aSlot, 0, nByte);
    }
  }
  return rc;
}

/*
** Free a hash table object.
*/
void sqlite3Fts5HashFree(Fts5Hash *pHash){
  if( pHash ){
    sqlite3Fts5HashClear(pHash);
    sqlite3_free(pHash->aSlot);
    sqlite3_free(pHash);
  }
}

/*
** Empty (but do not delete) a hash table.
*/
void sqlite3Fts5HashClear(Fts5Hash *pHash){
  int i;
  for(i=0; i<pHash->nSlot; i++){
    Fts5HashEntry *pNext;
    Fts5HashEntry *pSlot;
    for(pSlot=pHash->aSlot[i]; pSlot; pSlot=pNext){
      pNext = pSlot->pHashNext;
      sqlite3_free(pSlot);
    }
  }
  memset(pHash->aSlot, 0, pHash->nSlot * sizeof(Fts5HashEntry*));
  pHash->nEntry = 0;
}

static unsigned int fts5HashKey(int nSlot, const u8 *p, int n){
  int i;
  unsigned int h = 13;
  for(i=n-1; i>=0; i--){
    h = (h << 3) ^ h ^ p[i];
  }
  return (h % nSlot);
}

static unsigned int fts5HashKey2(int nSlot, u8 b, const u8 *p, int n){
  int i;
  unsigned int h = 13;
  for(i=n-1; i>=0; i--){
    h = (h << 3) ^ h ^ p[i];
  }
  h = (h << 3) ^ h ^ b;
  return (h % nSlot);
}

/*
** Resize the hash table by doubling the number of slots.
*/
static int fts5HashResize(Fts5Hash *pHash){
  int nNew = pHash->nSlot*2;
  int i;
  Fts5HashEntry **apNew;
  Fts5HashEntry **apOld = pHash->aSlot;

  apNew = (Fts5HashEntry**)sqlite3_malloc(nNew*sizeof(Fts5HashEntry*));
  if( !apNew ) return SQLITE_NOMEM;
  memset(apNew, 0, nNew*sizeof(Fts5HashEntry*));

  for(i=0; i<pHash->nSlot; i++){
    while( apOld[i] ){
      int iHash;
      Fts5HashEntry *p = apOld[i];
      apOld[i] = p->pHashNext;
      iHash = fts5HashKey(nNew, (u8*)p->zKey, strlen(p->zKey));
      p->pHashNext = apNew[iHash];
      apNew[iHash] = p;
    }
  }

  sqlite3_free(apOld);
  pHash->nSlot = nNew;
  pHash->aSlot = apNew;
  return SQLITE_OK;
}

static void fts5HashAddPoslistSize(Fts5HashEntry *p){
  if( p->iSzPoslist ){
    u8 *pPtr = (u8*)p;
    int nSz = (p->nData - p->iSzPoslist - 1);         /* Size in bytes */
    int nPos = nSz*2 + p->bDel;                       /* Value of nPos field */

    assert( p->bDel==0 || p->bDel==1 );
    if( nPos<=127 ){
      pPtr[p->iSzPoslist] = nPos;
    }else{
      int nByte = sqlite3Fts5GetVarintLen((u32)nPos);
      memmove(&pPtr[p->iSzPoslist + nByte], &pPtr[p->iSzPoslist + 1], nSz);
      sqlite3Fts5PutVarint(&pPtr[p->iSzPoslist], nPos);
      p->nData += (nByte-1);
    }
    p->bDel = 0;
    p->iSzPoslist = 0;
  }
}

int sqlite3Fts5HashWrite(
  Fts5Hash *pHash,
  i64 iRowid,                     /* Rowid for this entry */
  int iCol,                       /* Column token appears in (-ve -> delete) */
  int iPos,                       /* Position of token within column */
  char bByte,                     /* First byte of token */
  const char *pToken, int nToken  /* Token to add or remove to or from index */
){
  unsigned int iHash;
  Fts5HashEntry *p;
  u8 *pPtr;
  int nIncr = 0;                  /* Amount to increment (*pHash->pnByte) by */

  /* Attempt to locate an existing hash entry */
  iHash = fts5HashKey2(pHash->nSlot, (u8)bByte, (const u8*)pToken, nToken);
  for(p=pHash->aSlot[iHash]; p; p=p->pHashNext){
    if( p->zKey[0]==bByte 
     && memcmp(&p->zKey[1], pToken, nToken)==0 
     && p->zKey[nToken+1]==0 
    ){
      break;
    }
  }

  /* If an existing hash entry cannot be found, create a new one. */
  if( p==0 ){
    int nByte = FTS5_HASHENTRYSIZE + (nToken+1) + 1 + 64;
    if( nByte<128 ) nByte = 128;

    if( (pHash->nEntry*2)>=pHash->nSlot ){
      int rc = fts5HashResize(pHash);
      if( rc!=SQLITE_OK ) return rc;
      iHash = fts5HashKey2(pHash->nSlot, (u8)bByte, (const u8*)pToken, nToken);
    }

    p = (Fts5HashEntry*)sqlite3_malloc(nByte);
    if( !p ) return SQLITE_NOMEM;
    memset(p, 0, FTS5_HASHENTRYSIZE);
    p->nAlloc = nByte;
    p->zKey[0] = bByte;
    memcpy(&p->zKey[1], pToken, nToken);
    assert( iHash==fts5HashKey(pHash->nSlot, (u8*)p->zKey, nToken+1) );
    p->zKey[nToken+1] = '\0';
    p->nData = nToken+1 + 1 + FTS5_HASHENTRYSIZE;
    p->nData += sqlite3Fts5PutVarint(&((u8*)p)[p->nData], iRowid);
    p->iSzPoslist = p->nData;
    p->nData += 1;
    p->iRowid = iRowid;
    p->pHashNext = pHash->aSlot[iHash];
    pHash->aSlot[iHash] = p;
    pHash->nEntry++;
    nIncr += p->nData;
  }

  /* Check there is enough space to append a new entry. Worst case scenario
  ** is:
  **
  **     + 9 bytes for a new rowid,
  **     + 4 byte reserved for the "poslist size" varint.
  **     + 1 byte for a "new column" byte,
  **     + 3 bytes for a new column number (16-bit max) as a varint,
  **     + 5 bytes for the new position offset (32-bit max).
  */
  if( (p->nAlloc - p->nData) < (9 + 4 + 1 + 3 + 5) ){
    int nNew = p->nAlloc * 2;
    Fts5HashEntry *pNew;
    Fts5HashEntry **pp;
    pNew = (Fts5HashEntry*)sqlite3_realloc(p, nNew);
    if( pNew==0 ) return SQLITE_NOMEM;
    pNew->nAlloc = nNew;
    for(pp=&pHash->aSlot[iHash]; *pp!=p; pp=&(*pp)->pHashNext);
    *pp = pNew;
    p = pNew;
  }
  pPtr = (u8*)p;
  nIncr -= p->nData;

  /* If this is a new rowid, append the 4-byte size field for the previous
  ** entry, and the new rowid for this entry.  */
  if( iRowid!=p->iRowid ){
    fts5HashAddPoslistSize(p);
    p->nData += sqlite3Fts5PutVarint(&pPtr[p->nData], iRowid - p->iRowid);
    p->iSzPoslist = p->nData;
    p->nData += 1;
    p->iCol = 0;
    p->iPos = 0;
    p->iRowid = iRowid;
  }

  if( iCol>=0 ){
    /* Append a new column value, if necessary */
    assert( iCol>=p->iCol );
    if( iCol!=p->iCol ){
      pPtr[p->nData++] = 0x01;
      p->nData += sqlite3Fts5PutVarint(&pPtr[p->nData], iCol);
      p->iCol = iCol;
      p->iPos = 0;
    }

    /* Append the new position offset */
    p->nData += sqlite3Fts5PutVarint(&pPtr[p->nData], iPos - p->iPos + 2);
    p->iPos = iPos;
  }else{
    /* This is a delete. Set the delete flag. */
    p->bDel = 1;
  }
  nIncr += p->nData;

  *pHash->pnByte += nIncr;
  return SQLITE_OK;
}


/*
** Arguments pLeft and pRight point to linked-lists of hash-entry objects,
** each sorted in key order. This function merges the two lists into a
** single list and returns a pointer to its first element.
*/
static Fts5HashEntry *fts5HashEntryMerge(
  Fts5HashEntry *pLeft,
  Fts5HashEntry *pRight
){
  Fts5HashEntry *p1 = pLeft;
  Fts5HashEntry *p2 = pRight;
  Fts5HashEntry *pRet = 0;
  Fts5HashEntry **ppOut = &pRet;

  while( p1 || p2 ){
    if( p1==0 ){
      *ppOut = p2;
      p2 = 0;
    }else if( p2==0 ){
      *ppOut = p1;
      p1 = 0;
    }else{
      int i = 0;
      while( p1->zKey[i]==p2->zKey[i] ) i++;

      if( ((u8)p1->zKey[i])>((u8)p2->zKey[i]) ){
        /* p2 is smaller */
        *ppOut = p2;
        ppOut = &p2->pScanNext;
        p2 = p2->pScanNext;
      }else{
        /* p1 is smaller */
        *ppOut = p1;
        ppOut = &p1->pScanNext;
        p1 = p1->pScanNext;
      }
      *ppOut = 0;
    }
  }

  return pRet;
}

/*
** Extract all tokens from hash table iHash and link them into a list
** in sorted order. The hash table is cleared before returning. It is
** the responsibility of the caller to free the elements of the returned
** list.
*/
static int fts5HashEntrySort(
  Fts5Hash *pHash, 
  const char *pTerm, int nTerm,   /* Query prefix, if any */
  Fts5HashEntry **ppSorted
){
  const int nMergeSlot = 32;
  Fts5HashEntry **ap;
  Fts5HashEntry *pList;
  int iSlot;
  int i;

  *ppSorted = 0;
  ap = sqlite3_malloc(sizeof(Fts5HashEntry*) * nMergeSlot);
  if( !ap ) return SQLITE_NOMEM;
  memset(ap, 0, sizeof(Fts5HashEntry*) * nMergeSlot);

  for(iSlot=0; iSlot<pHash->nSlot; iSlot++){
    Fts5HashEntry *pIter;
    for(pIter=pHash->aSlot[iSlot]; pIter; pIter=pIter->pHashNext){
      if( pTerm==0
       || (strlen(pIter->zKey)>=nTerm && 0==memcmp(pIter->zKey, pTerm, nTerm))
      ){
        Fts5HashEntry *pEntry = pIter;
        pEntry->pScanNext = 0;
        for(i=0; ap[i]; i++){
          pEntry = fts5HashEntryMerge(pEntry, ap[i]);
          ap[i] = 0;
        }
        ap[i] = pEntry;
      }
    }
  }

  pList = 0;
  for(i=0; i<nMergeSlot; i++){
    pList = fts5HashEntryMerge(pList, ap[i]);
  }

  pHash->nEntry = 0;
  sqlite3_free(ap);
  *ppSorted = pList;
  return SQLITE_OK;
}

/*
** Query the hash table for a doclist associated with term pTerm/nTerm.
*/
int sqlite3Fts5HashQuery(
  Fts5Hash *pHash,                /* Hash table to query */
  const char *pTerm, int nTerm,   /* Query term */
  const u8 **ppDoclist,           /* OUT: Pointer to doclist for pTerm */
  int *pnDoclist                  /* OUT: Size of doclist in bytes */
){
  unsigned int iHash = fts5HashKey(pHash->nSlot, (const u8*)pTerm, nTerm);
  Fts5HashEntry *p;

  for(p=pHash->aSlot[iHash]; p; p=p->pHashNext){
    if( memcmp(p->zKey, pTerm, nTerm)==0 && p->zKey[nTerm]==0 ) break;
  }

  if( p ){
    fts5HashAddPoslistSize(p);
    *ppDoclist = (const u8*)&p->zKey[nTerm+1];
    *pnDoclist = p->nData - (FTS5_HASHENTRYSIZE + nTerm + 1);
  }else{
    *ppDoclist = 0;
    *pnDoclist = 0;
  }

  return SQLITE_OK;
}

int sqlite3Fts5HashScanInit(
  Fts5Hash *p,                    /* Hash table to query */
  const char *pTerm, int nTerm    /* Query prefix */
){
  return fts5HashEntrySort(p, pTerm, nTerm, &p->pScan);
}

void sqlite3Fts5HashScanNext(Fts5Hash *p){
  assert( !sqlite3Fts5HashScanEof(p) );
  p->pScan = p->pScan->pScanNext;
}

int sqlite3Fts5HashScanEof(Fts5Hash *p){
  return (p->pScan==0);
}

void sqlite3Fts5HashScanEntry(
  Fts5Hash *pHash,
  const char **pzTerm,            /* OUT: term (nul-terminated) */
  const u8 **ppDoclist,           /* OUT: pointer to doclist */
  int *pnDoclist                  /* OUT: size of doclist in bytes */
){
  Fts5HashEntry *p;
  if( (p = pHash->pScan) ){
    int nTerm = strlen(p->zKey);
    fts5HashAddPoslistSize(p);
    *pzTerm = p->zKey;
    *ppDoclist = (const u8*)&p->zKey[nTerm+1];
    *pnDoclist = p->nData - (FTS5_HASHENTRYSIZE + nTerm + 1);
  }else{
    *pzTerm = 0;
    *ppDoclist = 0;
    *pnDoclist = 0;
  }
}


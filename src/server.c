/*
** 2017 April 24
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/

#include "sqliteInt.h"

/*
** HMA file layout:
**
**      4 bytes - DMS slot. All connections read-lock this slot.
**
**   16*4 bytes - locking slots. Connections hold a read-lock on a locking slot
**                when they are connected, a write lock when they have an open
**                transaction.
**
**    N*4 bytes - Page locking slots. N is HMA_PAGELOCK_SLOTS.
**
** Page-locking slot format:
**
**   Each page-locking slot provides SHARED/RESERVED/EXCLUSIVE locks on a
**   single page. A RESERVED lock is similar to a RESERVED in SQLite's
**   rollback mode - existing SHARED locks may continue but new SHARED locks
**   may not be established. As in rollback mode, EXCLUSIVE and RESERVED 
**   locks are mutually exclusive.
**
**   Each 32-bit locking slot is divided into two sections - a bitmask for
**   read-locks and a single integer field for the write lock. The bitmask
**   occupies the least-significant 27 bits of the slot. The integer field
**   occupies the remaining 5 bits (so that it can store values from 0-31).
**
**   Each client has a unique integer client id. Currently these range from
**   0-15 (maximum of 16 concurrent connections). The page-locking slot format
**   allows this to be increased to 0-26 (maximum of 26 connections). To
**   take a SHARED lock, the corresponding bit is set in the locking slot
**   bitmask:
**
**     slot = slot | (1 << iClient);
**
**   To take an EXCLUSIVE or RESERVED lock, the integer part of the locking
**   slot is set to the client-id of the locker plus one (a value of zero 
**   indicates that no connection holds a RESERVED or EXCLUSIVE lock):
**
**     slot = slot | ((iClient+1) << 27)
*/

#ifdef SQLITE_SERVER_EDITION

#define HMA_CLIENT_SLOTS   16
#define HMA_PAGELOCK_SLOTS (256*1024)

#define HMA_FILE_SIZE (4 + 4*HMA_CLIENT_SLOTS + 4*HMA_PAGELOCK_SLOTS)

#include "unistd.h"
#include "fcntl.h"
#include "sys/mman.h"
#include "sys/types.h"
#include "sys/stat.h"
#include "errno.h"

typedef struct ServerHMA ServerHMA;

struct ServerGlobal {
  ServerHMA *pHma;                /* Linked list of all ServerHMA objects */
};
static struct ServerGlobal g_server;

/*
** There is one instance of the following structure for each distinct 
** HMA file opened by clients within this process. 
*/
struct ServerHMA {
  char *zName;                         /* hma file path */
  int fd;                              /* Fd open on hma file */
  int nClient;                         /* Current number of clients */
  Server *aClient[HMA_CLIENT_SLOTS];   /* Local (this process) clients */
  u32 *aMap;                           /* MMapped hma file */
  ServerHMA *pNext;                    /* Next HMA in this process */

  dev_t st_dev;
  ino_t st_ino;
};

struct Server {
  ServerHMA *pHma;                /* Hma file object */
  int iClient;                    /* Client id */
  Pager *pPager;                  /* Associated pager object */
  i64 nUsWrite;                   /* Cumulative us holding WRITER lock */
  i64 iUsWrite;                   /* Time WRITER lock was taken */
  int nAlloc;                     /* Allocated size of aLock[] array */
  int nLock;                      /* Number of entries in aLock[] */
  u32 *aLock;                     /* Mapped lock file */
};

#define SERVER_WRITE_LOCK 3
#define SERVER_READ_LOCK  2
#define SERVER_NO_LOCK    1

/*
** Global mutex functions used by code in this file.
*/
static void serverEnterMutex(void){
  sqlite3_mutex_enter(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_APP1));
}
static void serverLeaveMutex(void){
  sqlite3_mutex_leave(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_APP1));
}
static void serverAssertMutexHeld(void){
  assert( sqlite3_mutex_held(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_APP1)) );
}

static int posixLock(int fd, int iSlot, int eLock, int bBlock){
  int res;
  struct flock l;
  short aType[4] = {0, F_UNLCK, F_RDLCK, F_WRLCK};
  assert( eLock==SERVER_WRITE_LOCK 
       || eLock==SERVER_READ_LOCK 
       || eLock==SERVER_NO_LOCK 
  );
  memset(&l, 0, sizeof(l));
  l.l_type = aType[eLock];
  l.l_whence = SEEK_SET;
  l.l_start = iSlot*sizeof(u32);
  l.l_len = 1;

  res = fcntl(fd, (bBlock ? F_SETLKW : F_SETLK), &l);
  if( res && bBlock && errno==EDEADLK ){
    return SQLITE_BUSY_DEADLOCK;
  }
  return (res==0 ? SQLITE_OK : SQLITE_BUSY);
}

static int serverMapFile(ServerHMA *p){
  assert( p->aMap==0 );
  p->aMap = mmap(0, HMA_FILE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, p->fd, 0);
  if( p->aMap==0 ){
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}


static void serverDecrHmaRefcount(ServerHMA *pHma){
  if( pHma ){
    pHma->nClient--;
    if( pHma->nClient<=0 ){
      ServerHMA **pp;
      if( pHma->aMap ) munmap(pHma->aMap, HMA_FILE_SIZE);
      if( pHma->fd>=0 ) close(pHma->fd);
      for(pp=&g_server.pHma; *pp!=pHma; pp=&(*pp)->pNext);
      *pp = pHma->pNext;
      sqlite3_free(pHma);
    }
  }
}


static int serverOpenHma(Pager *pPager, const char *zPath, ServerHMA **ppHma){
  struct stat sStat;              /* Structure populated by stat() */
  int res;                        /* result of stat() */
  int rc = SQLITE_OK;             /* Return code */
  ServerHMA *pHma = 0;

  serverAssertMutexHeld();

  res = stat(zPath, &sStat);
  if( res!=0 ){
    sqlite3_log(SQLITE_CANTOPEN, "Failed to stat(%s)", zPath);
    rc = SQLITE_ERROR;
  }else{
    for(pHma=g_server.pHma; pHma; pHma=pHma->pNext){
      if( sStat.st_dev==pHma->st_dev && sStat.st_ino==pHma->st_ino ) break;
    }
    if( pHma==0 ){
      int nPath = strlen(zPath);
      int nByte = sizeof(ServerHMA) + nPath+1 + 4;

      pHma = (ServerHMA*)sqlite3_malloc(nByte);
      if( pHma==0 ){
        rc = SQLITE_NOMEM;
      }else{
        int i;
        memset(pHma, 0, nByte);
        pHma->zName = (char*)&pHma[1];
        pHma->nClient = 1;
        pHma->st_dev = sStat.st_dev;
        pHma->st_ino = sStat.st_ino;
        pHma->pNext = g_server.pHma;
        g_server.pHma = pHma;

        memcpy(pHma->zName, zPath, nPath);
        memcpy(&pHma->zName[nPath], "-hma", 5);

        pHma->fd = open(pHma->zName, O_RDWR|O_CREAT, 0644);
        if( pHma->fd<0 ){
          sqlite3_log(SQLITE_CANTOPEN, "Failed to open(%s)", pHma->zName);
          rc = SQLITE_ERROR;
        }

        if( rc==SQLITE_OK ){
          /* Write-lock the DMS slot. If successful, initialize the hma file. */
          rc = posixLock(pHma->fd, 0, SERVER_WRITE_LOCK, 0);
          if( rc==SQLITE_OK ){
            res = ftruncate(pHma->fd, HMA_FILE_SIZE);
            if( res!=0 ){
              sqlite3_log(SQLITE_CANTOPEN, 
                  "Failed to ftruncate(%s)", pHma->zName
              );
              rc = SQLITE_ERROR;
            }
            if( rc==SQLITE_OK ){
              rc = serverMapFile(pHma);
            }
            if( rc==SQLITE_OK ){
              memset(pHma->aMap, 0, HMA_FILE_SIZE);
            }else{
              rc = SQLITE_ERROR;
            }
            for(i=0; rc==SQLITE_OK && i<HMA_CLIENT_SLOTS; i++){
              rc = sqlite3PagerRollbackJournal(pPager, i);
            }
          }else{
            rc = serverMapFile(pHma);
          }
          if( rc==SQLITE_OK ){
            rc = posixLock(pHma->fd, 0, SERVER_READ_LOCK, 1);
          }
        }

        if( rc!=SQLITE_OK ){
          serverDecrHmaRefcount(pHma);
          pHma = 0;
        }
      }
    }else{
      pHma->nClient++;
    }
  }

  *ppHma = pHma;
  return rc;
}

static u32 *serverPageLockSlot(Server *p, Pgno pgno){
  int iSlot = pgno % HMA_PAGELOCK_SLOTS;
  return &p->pHma->aMap[1 + HMA_CLIENT_SLOTS + iSlot];
}
static u32 *serverClientSlot(Server *p, int iClient){
  return &p->pHma->aMap[1 + iClient];
}

/*
** Close the "connection" and *-hma file. This deletes the object passed
** as the first argument.
*/
void sqlite3ServerDisconnect(Server *p, sqlite3_file *dbfd){
  if( p->pHma ){
    ServerHMA *pHma = p->pHma;
    serverEnterMutex();
    if( p->iClient>=0 ){
      u32 *pSlot = serverClientSlot(p, p->iClient);
      *pSlot = 0;
      assert( pHma->aClient[p->iClient]==p );
      pHma->aClient[p->iClient] = 0;
      posixLock(pHma->fd, p->iClient+1, SERVER_NO_LOCK, 0);
    }
    if( dbfd 
     && pHma->nClient==1 
     && SQLITE_OK==sqlite3OsLock(dbfd, SQLITE_LOCK_EXCLUSIVE)
    ){
      unlink(pHma->zName);
    }
    serverDecrHmaRefcount(pHma);
    serverLeaveMutex();
  }
  sqlite3_free(p->aLock);
  sqlite3_free(p);
}

static int serverRollbackClient(Server *p, int iBlock){
  int rc;

  sqlite3_log(SQLITE_NOTICE, "Rolling back failed client %d", iBlock);

  /* Roll back any journal file for client iBlock. */
  rc = sqlite3PagerRollbackJournal(p->pPager, iBlock);

  /* Clear any locks held by client iBlock from the HMA file.  */
  if( rc==SQLITE_OK ){
    int i;
    for(i=0; i<HMA_PAGELOCK_SLOTS; i++){
      u32 *pSlot = serverPageLockSlot(p, (Pgno)i);
      u32 v = *pSlot;
      while( 1 ){
        u32 n = v & ~(1 << iBlock);
        if( (v>>HMA_CLIENT_SLOTS)==iBlock+1 ){
          n = n & ((1<<HMA_CLIENT_SLOTS)-1);
        }
        if( __sync_val_compare_and_swap(pSlot, v, n)==v ) break;
        v = *pSlot;
      }
    }
  }

  return rc;
}


/*
** Open the *-hma file and "connect" to the system.
*/
int sqlite3ServerConnect(
  Pager *pPager, 
  Server **ppOut, 
  int *piClient
){
  const char *zPath = sqlite3PagerFilename(pPager, 0);
  int rc = SQLITE_OK;
  Server *p;

  p = (Server*)sqlite3_malloc(sizeof(Server));
  if( p==0 ){
    rc = SQLITE_NOMEM;
  }else{
    memset(p, 0, sizeof(Server));
    p->iClient = -1;
    p->pPager = pPager;

    serverEnterMutex();
    rc = serverOpenHma(pPager, zPath, &p->pHma);

    /* File is now mapped. Find a free client slot. */
    if( rc==SQLITE_OK ){
      int i;
      Server **aClient = p->pHma->aClient;
      int fd = p->pHma->fd;
      for(i=0; i<HMA_CLIENT_SLOTS; i++){
        if( aClient[i]==0 ){
          int res = posixLock(fd, i+1, SERVER_WRITE_LOCK, 0);
          if( res==SQLITE_OK ){
            u32 *pSlot = serverClientSlot(p, i);
            if( *pSlot ){
              rc = serverRollbackClient(p, i);
            }
            posixLock(fd, i+1, (!rc ? SERVER_READ_LOCK : SERVER_NO_LOCK), 0);
            break;
          }
        }
      }

      if( rc==SQLITE_OK ){
        if( i>HMA_CLIENT_SLOTS ){
          rc = SQLITE_BUSY;
        }else{
          u32 *pSlot = serverClientSlot(p, i);
          *piClient = p->iClient = i;
          aClient[i] = p;
          *pSlot = 1;
        }
      }
    }

    serverLeaveMutex();
  }

  if( rc!=SQLITE_OK ){
    sqlite3ServerDisconnect(p, 0);
    p = 0;
  }
  *ppOut = p;
  return rc;
}

static int serverOvercomeLock(
  Server *p,                      /* Server connection */
  int bWrite,                     /* True for a write-lock */
  int bBlock,                     /* If true, block for this lock */
  u32 v,                          /* Value of blocking page locking slot */
  int *pbRetry                    /* OUT: True if caller should retry lock */
){
  int rc = SQLITE_OK;
  int iBlock = ((int)(v>>HMA_CLIENT_SLOTS))-1;

  if( iBlock<0 || iBlock==p->iClient ){
    for(iBlock=0; iBlock<HMA_CLIENT_SLOTS; iBlock++){
      if( iBlock!=p->iClient && (v & (1<<iBlock)) ) break;
    }
  }
  assert( iBlock<HMA_CLIENT_SLOTS );

  serverEnterMutex();

  if( 0==p->pHma->aClient[iBlock] ){
    rc = posixLock(p->pHma->fd, iBlock+1, SERVER_WRITE_LOCK, 0);
    if( rc==SQLITE_OK ){
      rc = serverRollbackClient(p, iBlock);

      /* Release the lock on slot iBlock */
      posixLock(p->pHma->fd, iBlock+1, SERVER_NO_LOCK, 0);
      if( rc==SQLITE_OK ){
        *pbRetry = 1;
      }
    }else if( rc==SQLITE_BUSY ){
      if( bBlock ){
        rc = posixLock(p->pHma->fd, iBlock+1, SERVER_READ_LOCK, 1);
        if( rc==SQLITE_OK ){
          posixLock(p->pHma->fd, iBlock+1, SERVER_NO_LOCK, 0);
          *pbRetry = 1;
        }
      }

      if( rc==SQLITE_BUSY ){
        rc = SQLITE_OK;
      }
    }
  }

  serverLeaveMutex();

  return rc;
}

/*
** Begin a transaction.
*/
int sqlite3ServerBegin(Server *p){
#if 1
  int rc = posixLock(p->pHma->fd, p->iClient+1, SERVER_WRITE_LOCK, 1);
  if( rc ) return rc;
#endif
  return sqlite3ServerLock(p, 1, 0, 1);
}

/*
** End a transaction (and release all locks).
*/
int sqlite3ServerEnd(Server *p){
  int i;
  for(i=0; i<p->nLock; i++){
    u32 *pSlot = serverPageLockSlot(p, p->aLock[i]);
    while( 1 ){
      u32 v = *pSlot;
      u32 n = v;
      if( (v>>HMA_CLIENT_SLOTS)==p->iClient+1 ){
        n = n & ((1 << HMA_CLIENT_SLOTS)-1);
      }
      n = n & ~(1 << p->iClient);
      if( __sync_val_compare_and_swap(pSlot, v, n)==v ) break;
    }
    if( p->aLock[i]==0 ){
      struct timeval t2;
      i64 nUs;
      gettimeofday(&t2, 0);
      nUs = (i64)t2.tv_sec * 1000000 + t2.tv_usec - p->iUsWrite; 
      p->nUsWrite += nUs;
      if( (p->nUsWrite / 1000000)!=((p->nUsWrite + nUs)/1000000) ){
        sqlite3_log(SQLITE_WARNING, 
            "Cumulative WRITER time: %lldms\n", p->nUsWrite/1000
        );
      }
    }
  }
  p->nLock = 0;
#if 1
  return posixLock(p->pHma->fd, p->iClient+1, SERVER_READ_LOCK, 0);
#endif
  return SQLITE_OK;
}

/*
** Release all write-locks.
*/
int sqlite3ServerReleaseWriteLocks(Server *p){
  int rc = SQLITE_OK;
  return rc;
}

/*
** Return the client id of the client that currently holds the EXCLUSIVE
** or RESERVED lock according to page-locking slot value v. Or -1 if no
** client holds such a lock.
*/
int serverWriteLocker(u32 v){
  return ((int)(v >> HMA_CLIENT_SLOTS)) - 1;
}

/*
** Lock page pgno for reading (bWrite==0) or writing (bWrite==1).
**
** If parameter bBlock is non-zero, then make this a blocking lock if
** possible.
*/
int sqlite3ServerLock(Server *p, Pgno pgno, int bWrite, int bBlock){
  int rc = SQLITE_OK;
  int bReserved = 0;
  u32 *pSlot = serverPageLockSlot(p, pgno);

  /* Grow the aLock[] array, if required */
  if( p->nLock==p->nAlloc ){
    int nNew = p->nAlloc ? p->nAlloc*2 : 128;
    u32 *aNew;
    aNew = (u32*)sqlite3_realloc(p->aLock, sizeof(u32)*nNew);
    if( aNew==0 ){
      rc = SQLITE_NOMEM_BKPT;
    }else{
      p->aLock = aNew;
      p->nAlloc = nNew;
    }
  }
  if( rc==SQLITE_OK ){
    u32 v = *pSlot;

    /* Check if the required lock is already held. If so, exit this function
    ** early. Otherwise, add an entry to the aLock[] array to record the fact
    ** that the lock may need to be released.  */
    if( bWrite ){
      int iLock = ((int)(v>>HMA_CLIENT_SLOTS)) - 1;
      if( iLock==p->iClient ) goto server_lock_out;
    }else{
      if( v & (1<<p->iClient) ) goto server_lock_out;
    }
    p->aLock[p->nLock++] = pgno;

    while( 1 ){
      u32 n;
      int w;
      u32 mask = (bWrite ? (((1<<HMA_CLIENT_SLOTS)-1) & ~(1<<p->iClient)) : 0);

      while( ((w = serverWriteLocker(v))>=0 && w!=p->iClient) || (v & mask) ){
        int bRetry = 0;

        if( w<0 && bWrite && bBlock ){
          /* Attempt a RESERVED lock before anything else */
          n = v | ((p->iClient+1) << HMA_CLIENT_SLOTS);
          assert( serverWriteLocker(n)==p->iClient );
          if( __sync_val_compare_and_swap(pSlot, v, n)!=v ){
            v = *pSlot;
            continue;
          }
          v = n;
          bReserved = 1;
        }

        rc = serverOvercomeLock(p, bWrite, bBlock, v, &bRetry);
        if( rc!=SQLITE_OK ) goto server_lock_out;
        if( bRetry==0 ){
          /* There is a conflicting lock. Cannot obtain this lock. */
          sqlite3_log(SQLITE_BUSY_DEADLOCK, "Conflict at page %d", (int)pgno);
          rc = SQLITE_BUSY_DEADLOCK;
          goto server_lock_out;
        }

        v = *pSlot;
      }

      n = v | (1 << p->iClient);
      if( bWrite ){
        n = n | ((p->iClient+1) << HMA_CLIENT_SLOTS);
      }
      if( __sync_val_compare_and_swap(pSlot, v, n)==v ) break;
      v = *pSlot;
    }
  }

server_lock_out:
  if( rc!=SQLITE_OK && bReserved ){
    u32 n;
    u32 v;
    do{
      v = *pSlot;
      assert( serverWriteLocker(v)==p->iClient );
      n = v & ((1<<HMA_CLIENT_SLOTS)-1);
    }while( __sync_val_compare_and_swap(pSlot, v, n)!=v );
  }

  if( pgno==0 ){
    struct timeval t1;
    gettimeofday(&t1, 0);
    p->iUsWrite = ((i64)t1.tv_sec * 1000000) + (i64)t1.tv_usec;
  }
  assert( rc!=SQLITE_OK || sqlite3ServerHasLock(p, pgno, bWrite) );
  return rc;
}

int sqlite3ServerHasLock(Server *p, Pgno pgno, int bWrite){
  u32 v = *serverPageLockSlot(p, pgno);
  if( bWrite ){
    return (v>>HMA_CLIENT_SLOTS)==(p->iClient+1);
  }
  return (v & (1 << p->iClient))!=0;
}

#endif /* ifdef SQLITE_SERVER_EDITION */
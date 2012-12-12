
#include "bson.h"


#include <regex.h>
#include "ejdb_private.h"
#include "ejdbutl.h"

/* private macros */
#define JBLOCKMETHOD(JB_ejdb, JB_wr)                            \
  ((JB_ejdb)->mmtx ? _ejdblockmethod((JB_ejdb), (JB_wr)) : true)
#define JBUNLOCKMETHOD(JB_ejdb)                         \
  ((JB_ejdb)->mmtx ? _ejdbunlockmethod(JB_ejdb) : true)

#define JBCLOCKMETHOD(JB_col, JB_wr)                            \
  ((JB_col)->mmtx ? _ejcollockmethod((JB_col), (JB_wr)) : true)
#define JBCUNLOCKMETHOD(JB_col)                         \
  ((JB_col)->mmtx ? _ejcollunlockmethod(JB_col) : true)

#define JBISOPEN(JB_jb) ((JB_jb) && (JB_jb)->metadb && (JB_jb)->metadb->open) ? true : false

#define JBISVALCOLNAME(JB_cname) ((JB_cname) && strlen(JB_cname) < JBMAXCOLNAMELEN && !index((JB_cname), '.'))

#define JBENSUREOPENLOCK(JB_jb, JB_lock, JB_ret)  \
                            assert(JB_jb); \
                            if (!JBLOCKMETHOD((JB_jb), (JB_lock))) return JB_ret; \
                            if (!JBISOPEN(JB_jb)) { \
                                _ejdbsetecode((JB_jb), TCEINVALID, __FILE__, __LINE__, __func__); \
                                JBUNLOCKMETHOD(JB_jb); \
                                return JB_ret; \
                            }

/* Default size of stack allocated buffer for string conversions eg. tcicaseformat() */
#define JBSTRINOPBUFFERSZ 512

/* Default size (16K) of tmp bson buffer on stack for field stripping in _pushstripbson() */
#define JBSBUFFERSZ 16384

/* string processing/conversion flags */
typedef enum {
    JBICASE = 1
} txtflags_t;

/* ejdb number */
typedef union {
    int64_t inum;
    double dnum;
} _EJDBNUM;

/* opaque data for `_bsonipathrowldr()` and `_bsonfpathrowldr()` functions */
typedef struct {
    EJCOLL *jcoll; //current collection
    bool icase; //ignore case normalization
} _BSONIPATHROWLDR;

/* context of deffered index updates. See `_updatebsonidx()` */
typedef struct {
    bson_oid_t oid;
    TCMAP *rmap;
    TCMAP *imap;
} _DEFFEREDIDXCTX;

/* private function prototypes */
static void _ejdbsetecode(EJDB *jb, int ecode, const char *filename, int line, const char *func);
static bool _ejdbsetmutex(EJDB *ejdb);
static bool _ejdblockmethod(EJDB *ejdb, bool wr);
static bool _ejdbunlockmethod(EJDB *ejdb);
static bool _ejdbcolsetmutex(EJCOLL *coll);
static bool _ejcollockmethod(EJCOLL *coll, bool wr);
static bool _ejcollunlockmethod(EJCOLL *coll);
static bson_type _bsonoidkey(bson *bs, bson_oid_t *oid);
static char* _bsonitstrval(EJDB *jb, bson_iterator *it, int *vsz, TCLIST *tokens, txtflags_t flags);
static char* _bsonipathrowldr(TCLIST *tokens, const char *pkbuf, int pksz, const char *rowdata, int rowdatasz,
        const char *ipath, int ipathsz, void *op, int *vsz);
static char* _bsonfpathrowldr(TCLIST *tokens, const char *rowdata, int rowdatasz,
        const char *fpath, int fpathsz, void *op, int *vsz);
static void _ejdbclear(EJDB *jb);
static bool _createcoldb(const char *colname, EJDB *jb, EJCOLLOPTS *opts, TCTDB** res);
static bool _addcoldb0(const char *colname, EJDB *jb, EJCOLLOPTS *opts, EJCOLL **res);
static void _delcoldb(EJCOLL *cdb);
static void _delqfdata(const EJQ *q, const EJQF *ejqf);
static bool _updatebsonidx(EJCOLL *jcoll, const bson_oid_t *oid, const bson *bs,
        const void *obsdata, int obsdatasz, TCLIST *dlist);
static bool _metasetopts(EJDB *jb, const char *colname, EJCOLLOPTS *opts);
static bool _metagetopts(EJDB *jb, const char *colname, EJCOLLOPTS *opts);
static bson* _metagetbson(EJDB *jb, const char *colname, int colnamesz, const char *mkey);
static bson* _metagetbson2(EJCOLL *jcoll, const char *mkey);
static bool _metasetbson(EJDB *jb, const char *colname, int colnamesz,
        const char *mkey, bson *val, bool merge, bool mergeoverwrt);
static bool _metasetbson2(EJCOLL *jcoll, const char *mkey, bson *val, bool merge, bool mergeoverwrt);
static bson* _imetaidx(EJCOLL *jcoll, const char *ipath);
static void _qrypreprocess(EJCOLL *jcoll, EJQ *ejq, int qflags, EJQF **mqf, TCMAP **ifields);
static TCLIST* _parseqobj(EJDB *jb, EJQ *q, bson *qspec);
static int _parse_qobj_impl(EJDB *jb, EJQ *q, bson_iterator *it, TCLIST *qlist, TCLIST *pathStack, EJQF *pqf);
static int _ejdbsoncmp(const TCLISTDATUM *d1, const TCLISTDATUM *d2, void *opaque);
static bool _qrycondcheckstrand(const char *vbuf, const TCLIST *tokens);
static bool _qrycondcheckstror(const char *vbuf, const TCLIST *tokens);
static bool _qrybsvalmatch(const EJQF *qf, bson_iterator *it, bool expandarrays);
static bool _qrybsmatch(const EJQF *qf, const void *bsbuf, int bsbufsz);
static bool _qryormatch(EJCOLL *jcoll, EJQ *ejq, const void *pkbuf, int pkbufsz, const void *bsbuf, int bsbufsz);
static bool _qryallcondsmatch(bool onlycount, int anum, EJCOLL *jcoll, const EJQF **qfs, int qfsz,
        const void *pkbuf, int pkbufsz, void **bsbuf, int *bsbufsz);
static void _qrydup(const EJQ *src, EJQ *target, uint32_t qflags);
static void _qrydel(EJQ *q, bool freequery);
static void _pushstripbson(TCLIST *rs, TCMAP *ifields, void *bsbuf, int bsbufsz);
static TCLIST* _qryexecute(EJCOLL *jcoll, const EJQ *q, uint32_t *count, int qflags, TCXSTR *log);
EJDB_INLINE void _nufetch(_EJDBNUM *nu, const char *sval, bson_type bt);
EJDB_INLINE int _nucmp(_EJDBNUM *nu, const char *sval, bson_type bt);
EJDB_INLINE int _nucmp2(_EJDBNUM *nu1, _EJDBNUM *nu2, bson_type bt);
static EJCOLL* _getcoll(EJDB *jb, const char *colname);


extern const char *utf8proc_errmsg(ssize_t errcode);

EJDB_EXPORT const char* ejdberrmsg(int ecode) {
    if (ecode > -6 && ecode < 0) { //Hook for negative error codes of utf8proc library
        return utf8proc_errmsg(ecode);
    }
    switch (ecode) {
        case JBEINVALIDCOLNAME: return "invalid collection name";
        case JBEINVALIDBSON: return "invalid bson object";
        case JBEQINVALIDQCONTROL: return "invalid query control field starting with '$'";
        case JBEQINOPNOTARRAY: return "$strand, $stror, $in, $nin, $bt keys requires not empty array value";
        case JBEMETANVALID: return "inconsistent database metadata";
        case JBEFPATHINVALID: return "invalid JSON field path value";
        case JBEQINVALIDQRX: return "invalid query regexp value";
        case JBEQRSSORTING: return "result set sorting error";
        case JBEQERROR: return "query generic error";
        case JBEQUPDFAILED: return "bson record update failed";
        case JBEINVALIDBSONPK: return "invalid bson _id field";
        default: return tcerrmsg(ecode);
    }
}

EJDB_EXPORT bool ejdbisvalidoidstr(const char *oid) {
    if (!oid) {
        return false;
    }
    int i = 0;
    for (; oid[i] != '\0' &&
            ((oid[i] >= 0x30 && oid[i] <= 0x39) || /* 1 - 9 */
            (oid[i] >= 0x61 && oid[i] <= 0x66)); ++i); /* a - f */
    return (i == 24);
}

/* Get the last happened error code of a database object. */
EJDB_EXPORT int ejdbecode(EJDB *jb) {
    assert(jb && jb->metadb);
    return tctdbecode(jb->metadb);
}

EJDB_EXPORT EJDB* ejdbnew(void) {
    EJDB *jb;
    TCMALLOC(jb, sizeof (*jb));
    _ejdbclear(jb);
    jb->metadb = tctdbnew();
    tctdbsetmutex(jb->metadb);
    tctdbsetcache(jb->metadb, 1024, 0, 0);
#ifdef _DEBUG
    tchdbsetdbgfd(jb->metadb->hdb, fileno(stderr));
#endif
    if (!_ejdbsetmutex(jb)) {
        tctdbdel(jb->metadb);
        TCFREE(jb);
        return NULL;
    }
    return jb;
}

EJDB_EXPORT void ejdbdel(EJDB *jb) {
    assert(jb && jb->metadb);
    if (JBISOPEN(jb)) ejdbclose(jb);
    for (int i = 0; i < jb->cdbsnum; ++i) {
        _delcoldb(jb->cdbs + i);
    }
    TCFREE(jb->cdbs);
    jb->cdbsnum = 0;
    if (jb->mmtx) {
        pthread_rwlock_destroy(jb->mmtx);
        TCFREE(jb->mmtx);
    }
    tctdbdel(jb->metadb);
    TCFREE(jb);
}

EJDB_EXPORT bool ejdbclose(EJDB *jb) {
    JBENSUREOPENLOCK(jb, true, false);
    bool rv = true;
    for (int i = 0; i < jb->cdbsnum; ++i) {
        JBCLOCKMETHOD(jb->cdbs + i, true);
        if (!tctdbclose(jb->cdbs[i].tdb)) {
            rv = false;
        }
        JBCUNLOCKMETHOD(jb->cdbs + i);
    }
    if (!tctdbclose(jb->metadb)) {
        rv = false;
    }
    JBUNLOCKMETHOD(jb);
    return rv;
}

EJDB_EXPORT bool ejdbisopen(EJDB *jb) {
    return JBISOPEN(jb);
}

EJDB_EXPORT bool ejdbopen(EJDB *jb, const char *path, int mode) {
    assert(jb && path);
    assert(jb->metadb);
    if (!JBLOCKMETHOD(jb, true)) return false;
    if (JBISOPEN(jb)) {
        _ejdbsetecode(jb, TCEINVALID, __FILE__, __LINE__, __func__);
        JBUNLOCKMETHOD(jb);
        return false;
    }
    bool rv = tctdbopen(jb->metadb, path, mode);
    if (!rv) {
        goto finish;
    }
    TCMALLOC(jb->cdbs, 1);
    jb->cdbsnum = 0;
    TCTDB *mdb = jb->metadb;
    rv = tctdbiterinit(mdb);
    if (!rv) {
        goto finish;
    }
    char *colname = NULL;
    for (int i = 0; i < mdb->hdb->rnum && (colname = tctdbiternext2(mdb)) != NULL; ++i) {
        EJCOLL *cdb;
        EJCOLLOPTS opts;
        _metagetopts(jb, colname, &opts);
        _addcoldb0(colname, jb, &opts, &cdb);
        TCFREE(colname);
    }
finish:
    JBUNLOCKMETHOD(jb);
    return rv;
}

EJDB_EXPORT EJCOLL* ejdbgetcoll(EJDB *jb, const char *colname) {
    assert(colname);
    EJCOLL *coll = NULL;
    JBENSUREOPENLOCK(jb, false, NULL);
    coll = _getcoll(jb, colname);
    JBUNLOCKMETHOD(jb);
    return coll;
}

EJDB_EXPORT EJCOLL* ejdbcreatecoll(EJDB *jb, const char *colname, EJCOLLOPTS *opts) {
    assert(colname);
    EJCOLL *coll = ejdbgetcoll(jb, colname);
    if (coll) {
        return coll;
    }
    if (!JBISVALCOLNAME(colname)) {
        _ejdbsetecode(jb, JBEINVALIDCOLNAME, __FILE__, __LINE__, __func__);
        return NULL;
    }
    JBENSUREOPENLOCK(jb, true, NULL);
    TCTDB *meta = jb->metadb;
    char *row = tcsprintf("name\t%s", colname);
    if (!tctdbput3(meta, colname, row)) {
        goto finish;
    }
    if (!_addcoldb0(colname, jb, opts, &coll)) {
        tctdbout2(meta, colname); //cleaning
        goto finish;
    }
    _metasetopts(jb, colname, opts);
finish:
    JBUNLOCKMETHOD(jb);
    if (row) {
        TCFREE(row);
    }
    return coll;
}

EJDB_EXPORT bool ejdbrmcoll(EJDB *jb, const char *colname, bool unlinkfile) {
    assert(colname);
    JBENSUREOPENLOCK(jb, true, false);
    bool rv = true;
    EJCOLL *coll = _getcoll(jb, colname);
    if (!coll) {
        goto finish;
    }
    EJCOLL *cdbs = jb->cdbs;
    for (int i = 0; i < jb->cdbsnum; ++i) {
        coll = jb->cdbs + i;
        if (!strcmp(colname, coll->cname)) {
            if (!JBCLOCKMETHOD(coll, true)) return false;
            jb->cdbsnum--;
            memmove(cdbs + i, cdbs + i + 1, sizeof (*cdbs) * (jb->cdbsnum - i));
            tctdbout2(jb->metadb, colname);
            tctdbvanish(coll->tdb);
            TCLIST *paths = tclistnew2(10);
            tclistpush2(paths, coll->tdb->hdb->path);
            for (int j = 0; j < coll->tdb->inum; ++j) {
                TDBIDX *idx = coll->tdb->idxs + j;
                const char *ipath = tcbdbpath(idx->db);
                if (ipath) {
                    tclistpush2(paths, ipath);
                }
            }
            tctdbclose(coll->tdb);
            if (unlinkfile) {
                for (int j = 0; j < TCLISTNUM(paths); ++j) {
                    unlink(tclistval2(paths, j));
                }
            }
            tclistdel(paths);
            JBCUNLOCKMETHOD(coll);
            _delcoldb(coll);
            break;
        }
    }
finish:
    JBUNLOCKMETHOD(jb);
    return rv;
}

/* Save/Update BSON  */
EJDB_EXPORT bool ejdbsavebson(EJCOLL *jcoll, bson *bs, bson_oid_t *oid) {
    return ejdbsavebson2(jcoll, bs, oid, false);
}

EJDB_EXPORT bool ejdbsavebson2(EJCOLL *jcoll, bson *bs, bson_oid_t *oid, bool merge) {
    assert(jcoll);
    if (!bs || bs->err || !bs->finished) {
        _ejdbsetecode(jcoll->jb, JBEINVALIDBSON, __FILE__, __LINE__, __func__);
        return false;
    }
    if (!JBISOPEN(jcoll->jb)) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    if (!JBCLOCKMETHOD(jcoll, true)) return false;
    bool rv = false;
    bson *nbs = NULL;
    bson_type oidt = _bsonoidkey(bs, oid);
    if (oidt == BSON_EOO) { //missing _id so generate a new _id
        bson_oid_gen(oid);
        nbs = bson_create();
        bson_init_size(nbs, bson_size(bs) + (strlen(JDBIDKEYNAME) + 1/*key*/ + 1/*type*/ + sizeof (*oid)));
        bson_append_oid(nbs, JDBIDKEYNAME, oid);
        bson_ensure_space(nbs, bson_size(bs) - 4);
        bson_append(nbs, bson_data(bs) + 4, bson_size(bs) - (4 + 1/*BSON_EOO*/));
        bson_finish(nbs);
        assert(!nbs->err);
        bs = nbs;
    } else if (oidt != BSON_OID) { //_oid presented by it is not BSON_OID
        _ejdbsetecode(jcoll->jb, JBEINVALIDBSONPK, __FILE__, __LINE__, __func__);
        return false;
    }
    TCTDB *tdb = jcoll->tdb;
    TCMAP *rowm = (tdb->hdb->rnum > 0) ? tctdbget(tdb, oid, sizeof (*oid)) : NULL;
    char *obsdata = NULL; //Old bson
    int obsdatasz = 0;
    if (rowm) { //Save the copy of old bson data
        const void *obs = tcmapget(rowm, JDBCOLBSON, JDBCOLBSONL, &obsdatasz);
        if (obs && obsdatasz > 0) {
            TCMALLOC(obsdata, obsdatasz);
            memmove(obsdata, obs, obsdatasz);
        }
    } else {
        rowm = tcmapnew2(TCMAPTINYBNUM);
    }
    if (merge && !nbs && obsdata) {
        nbs = bson_create();
        bson_init_size(nbs, MAX(obsdatasz, bson_size(bs)));
        bson_merge2(obsdata, bson_data(bs), true, nbs);
        bson_finish(nbs);
        assert(!nbs->err);
        bs = nbs;
    }
    tcmapput(rowm, JDBCOLBSON, JDBCOLBSONL, bson_data(bs), bson_size(bs));
    if (!tctdbput(tdb, oid, sizeof (*oid), rowm)) {
        goto finish;
    }
    //Update indexes
    rv = _updatebsonidx(jcoll, oid, bs, obsdata, obsdatasz, NULL);
finish:
    JBCUNLOCKMETHOD(jcoll);
    if (rowm) {
        tcmapdel(rowm);
    }
    if (obsdata) {
        TCFREE(obsdata);
    }
    if (nbs) {
        bson_del(nbs);
    }
    return rv;
}

EJDB_EXPORT bool ejdbrmbson(EJCOLL *jcoll, bson_oid_t *oid) {
    assert(jcoll && oid);
    if (!JBISOPEN(jcoll->jb)) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    JBCLOCKMETHOD(jcoll, true);
    bool rv = true;
    const void *olddata;
    int olddatasz = 0;
    TCMAP *rmap = tctdbget(jcoll->tdb, oid, sizeof (*oid));
    if (!rmap) {
        goto finish;
    }
    olddata = tcmapget3(rmap, JDBCOLBSON, JDBCOLBSONL, &olddatasz);
    if (!_updatebsonidx(jcoll, oid, NULL, olddata, olddatasz, NULL) ||
            !tctdbout(jcoll->tdb, oid, sizeof (*oid))) {
        rv = false;
    }
finish:
    JBCUNLOCKMETHOD(jcoll);
    if (rmap) {
        tcmapdel(rmap);
    }
    return rv;
}

EJDB_EXPORT bson* ejdbloadbson(EJCOLL *jcoll, const bson_oid_t *oid) {
    assert(jcoll && oid);
    if (!JBISOPEN(jcoll->jb)) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return NULL;
    }
    JBCLOCKMETHOD(jcoll, false);
    bson *ret = NULL;
    int datasz;
    void *cdata = tchdbget(jcoll->tdb->hdb, oid, sizeof (*oid), &datasz);
    if (!cdata) {
        goto finish;
    }
    void *bsdata = tcmaploadone(cdata, datasz, JDBCOLBSON, JDBCOLBSONL, &datasz);
    if (!bsdata) {
        goto finish;
    }
    if (datasz <= 4) {
        TCFREE(bsdata);
        goto finish;
    }
    ret = bson_create();
    bson_init_finished_data(ret, bsdata);
finish:
    JBCUNLOCKMETHOD(jcoll);
    if (cdata) {
        TCFREE(cdata);
    }
    return ret;
}

EJDB_EXPORT EJQ* ejdbcreatequery(EJDB *jb, bson *qobj, bson *orqobjs, int orqobjsnum, bson *hints) {
    assert(jb);
    if (!qobj || qobj->err || !qobj->finished) {
        _ejdbsetecode(jb, JBEINVALIDBSON, __FILE__, __LINE__, __func__);
        return NULL;
    }
    EJQ *q;
    TCCALLOC(q, 1, sizeof (*q));
    if (qobj) {
        q->qobjlist = _parseqobj(jb, q, qobj);
        if (!q->qobjlist) {
            goto error;
        }
    }
    if (orqobjs && orqobjsnum) {
        q->orqobjsnum = orqobjsnum;
        TCMALLOC(q->orqobjs, sizeof (*(q->orqobjs)) * q->orqobjsnum);
        for (int i = 0; i < orqobjsnum; ++i) {
            bson *oqb = (orqobjs + i);
            assert(oqb);
            EJQ *oq = ejdbcreatequery(jb, oqb, NULL, 0, NULL);
            if (oq == NULL) {
                goto error;
            }
            q->orqobjs[i] = *oq; //copy struct
            TCFREE(oq);
        }
    }
    if (hints) {
        if (hints->err || !hints->finished) {
            _ejdbsetecode(jb, JBEINVALIDBSON, __FILE__, __LINE__, __func__);
            return NULL;
        }
        q->hints = bson_create();
        if (bson_copy(q->hints, hints)) {
            goto error;
        }
    }
    return q;
error:
    ejdbquerydel(q);
    return NULL;
}

EJDB_EXPORT void ejdbquerydel(EJQ *q) {
    _qrydel(q, true);
}

/** Set index */
EJDB_EXPORT bool ejdbsetindex(EJCOLL *jcoll, const char *fpath, int flags) {
    assert(jcoll && fpath);
    bool rv = true;
    bson *imeta = NULL;
    bson_iterator it;
    int tcitype = 0; //TCDB index type
    int oldiflags = 0; //Old index flags stored in meta
    bool ibld = (flags & JBIDXREBLD);
    if (ibld) {
        flags &= ~JBIDXREBLD;
    }
    bool idrop = (flags & JBIDXDROP);
    if (idrop) {
        flags &= ~JBIDXDROP;
    }
    bool idropall = (flags & JBIDXDROPALL);
    if (idropall) {
        idrop = true;
        flags &= ~JBIDXDROPALL;
    }
    bool iop = (flags & JBIDXOP);
    if (iop) {
        flags &= ~JBIDXOP;
    }

    char ipath[BSON_MAX_FPATH_LEN + 2]; //add 2 bytes for one char prefix and '\0'term
    char ikey[BSON_MAX_FPATH_LEN + 2]; //add 2 bytes for one char prefix and '\0'term
    int fpathlen = strlen(fpath);
    if (fpathlen > BSON_MAX_FPATH_LEN) {
        _ejdbsetecode(jcoll->jb, JBEFPATHINVALID, __FILE__, __LINE__, __func__);
        rv = false;
        goto finish;
    }
    memmove(ikey + 1, fpath, fpathlen + 1);
    ikey[0] = 'i';
    memmove(ipath + 1, fpath, fpathlen + 1);
    ipath[0] = 's'; //defaulting to string index type

    JBENSUREOPENLOCK(jcoll->jb, true, false);
    imeta = _imetaidx(jcoll, fpath);
    if (!imeta) {
        if (idrop) { //Cannot drop/optimize not existent index;
            JBUNLOCKMETHOD(jcoll->jb);
            goto finish;
        }
        if (iop) {
            iop = false; //New index will be created
        }
        imeta = bson_create();
        bson_init(imeta);
        bson_append_string(imeta, "ipath", fpath);
        bson_append_int(imeta, "iflags", flags);
        bson_finish(imeta);
        rv = _metasetbson2(jcoll, ikey, imeta, false, false);
        if (!rv) {
            JBUNLOCKMETHOD(jcoll->jb);
            goto finish;
        }
    } else {
        if (bson_find(&it, imeta, "iflags") != BSON_EOO) {
            oldiflags = bson_iterator_int(&it);
        }
        if (!idrop && oldiflags != flags) { //Update index meta
            bson imetadelta;
            bson_init(&imetadelta);
            bson_append_int(&imetadelta, "iflags", flags);
            bson_finish(&imetadelta);
            rv = _metasetbson2(jcoll, ikey, &imetadelta, true, true);
            bson_destroy(&imetadelta);
            if (!rv) {
                JBUNLOCKMETHOD(jcoll->jb);
                goto finish;
            }
        }
    }
    JBUNLOCKMETHOD(jcoll->jb);

    if (idrop) {
        tcitype = TDBITVOID;
        if (idropall && oldiflags) {
            flags = oldiflags; //Drop index only for existing types
        }
    } else if (iop) {
        tcitype = TDBITOPT;
        if (oldiflags) {
            flags = oldiflags; //Optimize index for all existing types
        }
    }

    if (!JBCLOCKMETHOD(jcoll, true)) {
        rv = false;
        goto finish;
    }
    _BSONIPATHROWLDR op;
    op.icase = false;
    op.jcoll = jcoll;

    if (tcitype) {
        if (flags & JBIDXSTR) {
            ipath[0] = 's';
            rv = tctdbsetindexrldr(jcoll->tdb, ipath, tcitype, _bsonipathrowldr, &op);
        }
        if (flags & JBIDXISTR) {
            ipath[0] = 'i';
            op.icase = true;
            rv = tctdbsetindexrldr(jcoll->tdb, ipath, tcitype, _bsonipathrowldr, &op);
        }
        if (rv && (flags & JBIDXNUM)) {
            ipath[0] = 'n';
            rv = tctdbsetindexrldr(jcoll->tdb, ipath, tcitype, _bsonipathrowldr, &op);
        }
        if (rv && (flags & JBIDXARR)) {
            ipath[0] = 'a';
            rv = tctdbsetindexrldr(jcoll->tdb, ipath, tcitype, _bsonipathrowldr, &op);
        }
        if (idrop) { //Update index meta on drop
            oldiflags &= ~flags;
            if (oldiflags) { //Index dropped only for some types
                bson imetadelta;
                bson_init(&imetadelta);
                bson_append_int(&imetadelta, "iflags", oldiflags);
                bson_finish(&imetadelta);
                rv = _metasetbson2(jcoll, ikey, &imetadelta, true, true);
                bson_destroy(&imetadelta);
            } else { //Index dropped completely
                rv = _metasetbson2(jcoll, ikey, NULL, false, false);
            }
        }
    } else {
        if ((flags & JBIDXSTR) && (ibld || !(oldiflags & JBIDXSTR))) {
            ipath[0] = 's';
            rv = tctdbsetindexrldr(jcoll->tdb, ipath, TDBITLEXICAL, _bsonipathrowldr, &op);
        }
        if ((flags & JBIDXISTR) && (ibld || !(oldiflags & JBIDXISTR))) {
            ipath[0] = 'i';
            op.icase = true;
            rv = tctdbsetindexrldr(jcoll->tdb, ipath, TDBITLEXICAL, _bsonipathrowldr, &op);
        }
        if (rv && (flags & JBIDXNUM) && (ibld || !(oldiflags & JBIDXNUM))) {
            ipath[0] = 'n';
            rv = tctdbsetindexrldr(jcoll->tdb, ipath, TDBITDECIMAL, _bsonipathrowldr, &op);
        }
        if (rv && (flags & JBIDXARR) && (ibld || !(oldiflags & JBIDXARR))) {
            ipath[0] = 'a';
            rv = tctdbsetindexrldr(jcoll->tdb, ipath, TDBITTOKEN, _bsonipathrowldr, &op);
        }
    }
    JBCUNLOCKMETHOD(jcoll);
finish:
    if (imeta) {
        bson_del(imeta);
    }
    return rv;
}

EJDB_EXPORT uint32_t ejdbupdate(EJCOLL *jcoll, bson *qobj, bson *orqobjs, int orqobjsnum, bson *hints, TCXSTR *log) {
    assert(jcoll);
    uint32_t count = 0;
    EJQ *q = ejdbcreatequery(jcoll->jb, qobj, orqobjs, orqobjsnum, hints);
    if (q == NULL) {
        return count;
    }
    ejdbqryexecute(jcoll, q, &count, JBQRYCOUNT, log);
    ejdbquerydel(q);
    return count;
}

EJDB_EXPORT TCLIST* ejdbqryexecute(EJCOLL *jcoll, const EJQ *q, uint32_t *count, int qflags, TCXSTR *log) {
    assert(jcoll && q && q->qobjlist);
    if (!JBISOPEN(jcoll->jb)) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return NULL;
    }
    JBCLOCKMETHOD(jcoll, (q->flags & EJQUPDATING) ? true : false);
    TCLIST *res;
    _ejdbsetecode(jcoll->jb, TCESUCCESS, __FILE__, __LINE__, __func__);
    res = _qryexecute(jcoll, q, count, qflags, log);
    JBCUNLOCKMETHOD(jcoll);
    return res;
}

EJDB_EXPORT bool ejdbsyncoll(EJCOLL *jcoll) {
    assert(jcoll);
    if (!JBISOPEN(jcoll->jb)) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    bool rv = false;
    if (!JBCLOCKMETHOD(jcoll, true)) return false;
    rv = tctdbsync(jcoll->tdb);
    JBCUNLOCKMETHOD(jcoll);
    return rv;
}

EJDB_EXPORT bool ejdbsyncdb(EJDB *jb) {
    assert(jb);
    JBENSUREOPENLOCK(jb, true, false);
    bool rv = true;
    EJCOLL *coll = NULL;
    for (int i = 0; i < jb->cdbsnum; ++i) {
        coll = jb->cdbs + i;
        assert(coll);
        rv = JBCLOCKMETHOD(coll, true);
        if (!rv) break;
        rv = tctdbsync(coll->tdb);
        JBCUNLOCKMETHOD(coll);
        if (!rv) break;
    }
    JBUNLOCKMETHOD(jb);
    return rv;
}

EJDB_EXPORT bool ejdbtranbegin(EJCOLL *jcoll) {
    assert(jcoll);
    if (!JBISOPEN(jcoll->jb)) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    for (double wsec = 1.0 / sysconf(_SC_CLK_TCK); true; wsec *= 2) {
        if (!JBCLOCKMETHOD(jcoll, true)) return false;
        if (!jcoll->tdb->open || !jcoll->tdb->wmode) {
            _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
            JBCUNLOCKMETHOD(jcoll);
            return false;
        }
        if (!jcoll->tdb->tran) break;
        JBCUNLOCKMETHOD(jcoll);
        if (wsec > 1.0) wsec = 1.0;
        tcsleep(wsec);
    }
    if (!tctdbtranbeginimpl(jcoll->tdb)) {
        JBCUNLOCKMETHOD(jcoll);
        return false;
    }
    jcoll->tdb->tran = true;
    JBCUNLOCKMETHOD(jcoll);
    return true;
}

EJDB_EXPORT bool ejdbtrancommit(EJCOLL *jcoll) {
    assert(jcoll);
    if (!JBISOPEN(jcoll->jb)) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    if (!JBCLOCKMETHOD(jcoll, true)) return false;
    if (!jcoll->tdb->open || !jcoll->tdb->wmode || !jcoll->tdb->tran) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        JBCUNLOCKMETHOD(jcoll);
        return false;
    }
    jcoll->tdb->tran = false;
    bool err = false;
    if (!tctdbtrancommitimpl(jcoll->tdb)) err = true;
    JBCUNLOCKMETHOD(jcoll);
    return !err;
}

EJDB_EXPORT bool ejdbtranabort(EJCOLL *jcoll) {
    assert(jcoll);
    if (!JBISOPEN(jcoll->jb)) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    if (!JBCLOCKMETHOD(jcoll, true)) return false;
    if (!jcoll->tdb->open || !jcoll->tdb->wmode || !jcoll->tdb->tran) {
        _ejdbsetecode(jcoll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        JBCUNLOCKMETHOD(jcoll);
        return false;
    }
    jcoll->tdb->tran = false;
    bool err = false;
    if (!tctdbtranabortimpl(jcoll->tdb)) err = true;
    JBCUNLOCKMETHOD(jcoll);
    return !err;
}

/*************************************************************************************************
 * private features
 *************************************************************************************************/

/* Set the error code of a table database object. */
static void _ejdbsetecode(EJDB *jb, int ecode, const char *filename, int line, const char *func) {
    assert(jb && filename && line >= 1 && func);
    tctdbsetecode(jb->metadb, ecode, filename, line, func);
}

static EJCOLL* _getcoll(EJDB *jb, const char *colname) {
    assert(colname);
    EJCOLL *coll = NULL;
    //check if collection exists
    for (int i = 0; i < jb->cdbsnum; ++i) {
        coll = jb->cdbs + i;
        assert(coll);
        if (!strcmp(colname, coll->cname)) {
            break;
        } else {
            coll = NULL;
        }
    }
    return coll;
}

/* Set mutual exclusion control of a table database object for threading. */
static bool _ejdbsetmutex(EJDB *ejdb) {
    assert(ejdb);
    if (!TCUSEPTHREAD) return true;
    if (ejdb->mmtx || JBISOPEN(ejdb)) {
        _ejdbsetecode(ejdb, TCEINVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    TCMALLOC(ejdb->mmtx, sizeof (pthread_rwlock_t));
    bool err = false;
    if (pthread_rwlock_init(ejdb->mmtx, NULL) != 0) err = true;
    if (err) {
        TCFREE(ejdb->mmtx);
        ejdb->mmtx = NULL;
        return false;
    }
    return true;
}

/* Lock a method of the table database object.
   `tdb' specifies the table database object.
   `wr' specifies whether the lock is writer or not.
   If successful, the return value is true, else, it is false. */
static bool _ejdblockmethod(EJDB *ejdb, bool wr) {
    assert(ejdb);
    if (wr ? pthread_rwlock_wrlock(ejdb->mmtx) != 0 : pthread_rwlock_rdlock(ejdb->mmtx) != 0) {
        _ejdbsetecode(ejdb, TCETHREAD, __FILE__, __LINE__, __func__);
        return false;
    }
    TCTESTYIELD();
    return true;
}

/* Unlock a method of the table database object.
   `tdb' specifies the table database object.
   If successful, the return value is true, else, it is false. */
static bool _ejdbunlockmethod(EJDB *ejdb) {
    assert(ejdb);
    if (pthread_rwlock_unlock(ejdb->mmtx) != 0) {
        _ejdbsetecode(ejdb, TCETHREAD, __FILE__, __LINE__, __func__);
        return false;
    }
    TCTESTYIELD();
    return true;
}

static bool _ejdbcolsetmutex(EJCOLL *coll) {
    assert(coll && coll->jb);
    if (!TCUSEPTHREAD) return true;
    if (coll->mmtx) {
        _ejdbsetecode(coll->jb, TCEINVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    TCMALLOC(coll->mmtx, sizeof (pthread_rwlock_t));
    bool err = false;
    if (pthread_rwlock_init(coll->mmtx, NULL) != 0) err = true;
    if (err) {
        TCFREE(coll->mmtx);
        coll->mmtx = NULL;
        return false;
    }
    return true;
}

static bool _ejcollockmethod(EJCOLL *coll, bool wr) {
    assert(coll && coll->jb);
    if (wr ? pthread_rwlock_wrlock(coll->mmtx) != 0 : pthread_rwlock_rdlock(coll->mmtx) != 0) {
        _ejdbsetecode(coll->jb, TCETHREAD, __FILE__, __LINE__, __func__);
        return false;
    }
    TCTESTYIELD();
    return (coll->tdb && coll->tdb->open);
}

static bool _ejcollunlockmethod(EJCOLL *coll) {
    assert(coll && coll->jb);
    if (pthread_rwlock_unlock(coll->mmtx) != 0) {
        _ejdbsetecode(coll->jb, TCETHREAD, __FILE__, __LINE__, __func__);
        return false;
    }
    TCTESTYIELD();
    return true;
}

static void _qrydel(EJQ *q, bool freequery) {
    if (!q) {
        return;
    }
    const EJQF *qf = NULL;
    if (q->qobjlist) {
        for (int i = 0; i < TCLISTNUM(q->qobjlist); ++i) {
            qf = TCLISTVALPTR(q->qobjlist, i);
            assert(qf);
            _delqfdata(q, qf);
        }
        tclistdel(q->qobjlist);
        q->qobjlist = NULL;
    }
    if (q->orqobjsnum) {
        for (int i = 0; i < q->orqobjsnum; ++i) {
            _qrydel(q->orqobjs + i, false);
        }
        TCFREE(q->orqobjs);
        q->orqobjsnum = 0;
        q->orqobjs = NULL;
    }
    if (q->hints) {
        bson_del(q->hints);
        q->hints = NULL;
    }
    if (freequery) {
        TCFREE(q);
    }
}

static bool _qrybsvalmatch(const EJQF *qf, bson_iterator *it, bool expandarrays) {
    if (qf->tcop == TDBQTRUE) {
        return (true == !qf->negate);
    }
    bool rv = false;
    bson_type bt = bson_iterator_type(it);
    const char *expr = qf->expr;
    int exprsz = qf->exprsz;
    char sbuf[JBSTRINOPBUFFERSZ]; //buffer for icase comparisons
    char oidbuf[25]; //OID buffer
    char *cbuf = NULL;
    int cbufstrlen = 0;
    int fvalsz;
    int sp;
    const char *fval;

#define _FETCHSTRFVAL() \
    do { \
        fvalsz = (bt == BSON_STRING) ? bson_iterator_string_len(it) : 1; \
        fval = (bt == BSON_STRING) ? bson_iterator_string(it) : ""; \
        if (bt == BSON_OID) { \
            bson_oid_to_string(bson_iterator_oid(it), oidbuf); \
            fvalsz = 25; \
            fval = oidbuf; \
        } \
    } while(false)


    if (bt == BSON_ARRAY && expandarrays) { //Iterate over array
        bson_iterator sit;
        bson_iterator_subiterator(it, &sit);
        while ((bt = bson_iterator_next(&sit)) != BSON_EOO) {
            if (_qrybsvalmatch(qf, &sit, false)) {
                return true;
            }
        }
        return (false == !qf->negate);
    }
    switch (qf->tcop) {
        case TDBQCSTREQ:
        {
            _FETCHSTRFVAL();
            if ((qf->flags & EJCONDICASE) && (bt == BSON_STRING)) {
                cbufstrlen = tcicaseformat(fval, fvalsz - 1, sbuf, JBSTRINOPBUFFERSZ, &cbuf);
                if (cbufstrlen < 0) {
                    _ejdbsetecode(qf->jb, cbufstrlen, __FILE__, __LINE__, __func__);
                    rv = false;
                } else {
                    rv = (exprsz == cbufstrlen) && (exprsz == 0 || !memcmp(expr, cbuf, exprsz));
                }
                if (cbuf && cbuf != sbuf) {
                    TCFREE(cbuf);
                }
            } else {
                rv = (exprsz == fvalsz - 1) && (exprsz == 0 || !memcmp(expr, fval, exprsz));
            }
            break;
        }
        case TDBQCSTRINC:
        {
            _FETCHSTRFVAL();
            if ((qf->flags & EJCONDICASE) && (bt == BSON_STRING)) {
                cbufstrlen = tcicaseformat(fval, fvalsz - 1, sbuf, JBSTRINOPBUFFERSZ, &cbuf);
                if (cbufstrlen < 0) {
                    _ejdbsetecode(qf->jb, cbufstrlen, __FILE__, __LINE__, __func__);
                    rv = false;
                } else {
                    rv = (exprsz <= cbufstrlen) && strstr(cbuf, expr);
                }
                if (cbuf && cbuf != sbuf) {
                    TCFREE(cbuf);
                }
            } else {
                rv = (exprsz <= fvalsz) && strstr(fval, expr);
            }
            break;
        }
        case TDBQCSTRBW:
        {
            _FETCHSTRFVAL();
            if ((qf->flags & EJCONDICASE) && (bt == BSON_STRING)) {
                cbufstrlen = tcicaseformat(fval, fvalsz - 1, sbuf, JBSTRINOPBUFFERSZ, &cbuf);
                if (cbufstrlen < 0) {
                    _ejdbsetecode(qf->jb, cbufstrlen, __FILE__, __LINE__, __func__);
                    rv = false;
                } else {
                    rv = tcstrfwm(cbuf, expr);
                }
                if (cbuf && cbuf != sbuf) {
                    TCFREE(cbuf);
                }
            } else {
                rv = tcstrfwm(fval, expr);
            }
            break;
        }
        case TDBQCSTREW:
        {
            _FETCHSTRFVAL();
            if ((qf->flags & EJCONDICASE) && (bt == BSON_STRING)) {
                cbufstrlen = tcicaseformat(fval, fvalsz - 1, sbuf, JBSTRINOPBUFFERSZ, &cbuf);
                if (cbufstrlen < 0) {
                    _ejdbsetecode(qf->jb, cbufstrlen, __FILE__, __LINE__, __func__);
                    rv = false;
                } else {
                    rv = tcstrbwm(cbuf, expr);
                }
                if (cbuf && cbuf != sbuf) {
                    TCFREE(cbuf);
                }
            } else {
                rv = tcstrbwm(fval, expr);
            }
            break;
        }
        case TDBQCSTRAND:
        {
            TCLIST *tokens = qf->exprlist;
            assert(tokens);
            _FETCHSTRFVAL();
            if ((qf->flags & EJCONDICASE) && (bt == BSON_STRING)) {
                cbufstrlen = tcicaseformat(fval, fvalsz - 1, sbuf, JBSTRINOPBUFFERSZ, &cbuf);
                if (cbufstrlen < 0) {
                    _ejdbsetecode(qf->jb, cbufstrlen, __FILE__, __LINE__, __func__);
                    rv = false;
                } else {
                    rv = _qrycondcheckstrand(cbuf, tokens);
                }
                if (cbuf && cbuf != sbuf) {
                    TCFREE(cbuf);
                }
            } else {
                rv = _qrycondcheckstrand(fval, tokens);
            }
            break;
        }
        case TDBQCSTROR:
        {
            TCLIST *tokens = qf->exprlist;
            assert(tokens);
            _FETCHSTRFVAL();
            if ((qf->flags & EJCONDICASE) && (bt == BSON_STRING)) {
                cbufstrlen = tcicaseformat(fval, fvalsz - 1, sbuf, JBSTRINOPBUFFERSZ, &cbuf);
                if (cbufstrlen < 0) {
                    _ejdbsetecode(qf->jb, cbufstrlen, __FILE__, __LINE__, __func__);
                    rv = false;
                } else {
                    rv = _qrycondcheckstror(cbuf, tokens);
                }
                if (cbuf && cbuf != sbuf) {
                    TCFREE(cbuf);
                }
            } else {
                rv = _qrycondcheckstror(fval, tokens);
            }
            break;
        }
        case TDBQCSTROREQ:
        {
            TCLIST *tokens = qf->exprlist;
            assert(tokens);
            _FETCHSTRFVAL();
            if ((qf->flags & EJCONDICASE) && (bt == BSON_STRING)) {
                cbufstrlen = tcicaseformat(fval, fvalsz - 1, sbuf, JBSTRINOPBUFFERSZ, &cbuf);
                if (cbufstrlen < 0) {
                    _ejdbsetecode(qf->jb, cbufstrlen, __FILE__, __LINE__, __func__);
                    rv = false;
                } else {
                    if (qf->exprmap) {
                        if (tcmapget(qf->exprmap, cbuf, cbufstrlen, &sp) != NULL) {
                            rv = true;
                            break;
                        }
                    } else {
                        for (int i = 0; i < TCLISTNUM(tokens); ++i) {
                            const char *token = TCLISTVALPTR(tokens, i);
                            int tokensz = TCLISTVALSIZ(tokens, i);
                            if (tokensz == cbufstrlen && !strncmp(token, cbuf, tokensz)) {
                                rv = true;
                                break;
                            }
                        }
                    }
                }
                if (cbuf && cbuf != sbuf) {
                    TCFREE(cbuf);
                }
            } else {
                if (qf->exprmap) {
                    if (tcmapget3(qf->exprmap, fval, (fvalsz - 1), &sp) != NULL) {
                        rv = true;
                        break;
                    }
                } else {
                    for (int i = 0; i < TCLISTNUM(tokens); ++i) {
                        const char *token = TCLISTVALPTR(tokens, i);
                        int tokensz = TCLISTVALSIZ(tokens, i);
                        if (tokensz == (fvalsz - 1) && !strncmp(token, fval, tokensz)) {
                            rv = true;
                            break;
                        }
                    }
                }
            }
            break;
        }
        case TDBQCSTRORBW:
        {
            TCLIST *tokens = qf->exprlist;
            assert(tokens);
            _FETCHSTRFVAL();
            if ((qf->flags & EJCONDICASE) && (bt == BSON_STRING)) {
                cbufstrlen = tcicaseformat(fval, fvalsz - 1, sbuf, JBSTRINOPBUFFERSZ, &cbuf);
                if (cbufstrlen < 0) {
                    _ejdbsetecode(qf->jb, cbufstrlen, __FILE__, __LINE__, __func__);
                    rv = false;
                } else {
                    for (int i = 0; i < TCLISTNUM(tokens); ++i) {
                        const char *token = TCLISTVALPTR(tokens, i);
                        int tokensz = TCLISTVALSIZ(tokens, i);
                        if (tokensz <= cbufstrlen && !strncmp(token, cbuf, tokensz)) {
                            rv = true;
                            break;
                        }
                    }
                }
                if (cbuf && cbuf != sbuf) {
                    TCFREE(cbuf);
                }
            } else {
                for (int i = 0; i < TCLISTNUM(tokens); ++i) {
                    const char *token = TCLISTVALPTR(tokens, i);
                    int tokensz = TCLISTVALSIZ(tokens, i);
                    if (tokensz <= (fvalsz - 1) && !strncmp(token, fval, tokensz)) {
                        rv = true;
                        break;
                    }
                }
            }
            break;
        }
        case TDBQCSTRRX:
        {
            _FETCHSTRFVAL();
            rv = qf->regex && (regexec(qf->regex, fval, 0, NULL, 0) == 0);
            break;
        }
        case TDBQCNUMEQ:
        {
            if (bt == BSON_DOUBLE) {
                rv = (qf->exprdblval == bson_iterator_double_raw(it));
            } else if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
                rv = (qf->exprlongval == bson_iterator_long(it));
            } else {
                rv = false;
            }
            break;
        }
        case TDBQCNUMGT:
        {
            if (bt == BSON_DOUBLE) {
                rv = (qf->exprdblval < bson_iterator_double_raw(it));
            } else if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
                rv = (qf->exprlongval < bson_iterator_long(it));
            } else {
                rv = false;
            }
            break;
        }
        case TDBQCNUMGE:
        {
            if (bt == BSON_DOUBLE) {
                rv = (qf->exprdblval <= bson_iterator_double_raw(it));
            } else if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
                rv = (qf->exprlongval <= bson_iterator_long(it));
            } else {
                rv = false;
            }
            break;
        }
        case TDBQCNUMLT:
        {
            if (bt == BSON_DOUBLE) {
                rv = (qf->exprdblval > bson_iterator_double_raw(it));
            } else if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
                rv = (qf->exprlongval > bson_iterator_long(it));
            } else {
                rv = false;
            }
            break;
        }
        case TDBQCNUMLE:
        {
            if (bt == BSON_DOUBLE) {
                rv = (qf->exprdblval >= bson_iterator_double_raw(it));
            } else if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
                rv = (qf->exprlongval >= bson_iterator_long(it));
            } else {
                rv = false;
            }
            break;
        }
        case TDBQCNUMBT:
        {
            assert(qf->ftype == BSON_ARRAY);
            TCLIST *tokens = qf->exprlist;
            assert(tokens);
            assert(TCLISTNUM(tokens) == 2);
            if (bson_iterator_type(it) == BSON_DOUBLE) {
                double v1 = tctdbatof2(tclistval2(tokens, 0));
                double v2 = tctdbatof2(tclistval2(tokens, 1));
                double val = bson_iterator_double(it);
                rv = (v2 > v1) ? (v2 >= val && v1 <= val) : (v2 <= val && v1 >= val);
            } else {
                int64_t v1 = tctdbatoi(tclistval2(tokens, 0));
                int64_t v2 = tctdbatoi(tclistval2(tokens, 1));
                int64_t val = bson_iterator_long(it);
                rv = (v2 > v1) ? (v2 >= val && v1 <= val) : (v2 <= val && v1 >= val);
            }
            break;
        }
        case TDBQCNUMOREQ:
        {
            TCLIST *tokens = qf->exprlist;
            assert(tokens);
            if (bt == BSON_DOUBLE) {
                double nval = bson_iterator_double_raw(it);
                for (int i = 0; i < TCLISTNUM(tokens); ++i) {
                    if (tctdbatof2(TCLISTVALPTR(tokens, i)) == nval) {
                        rv = true;
                        break;
                    }
                }
            } else if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
                int64_t nval = bson_iterator_long(it);
                for (int i = 0; i < TCLISTNUM(tokens); ++i) {
                    if (tctdbatoi(TCLISTVALPTR(tokens, i)) == nval) {
                        rv = true;
                        break;
                    }
                }
            }
            break;
        }
    }
    return (rv == !qf->negate);

#undef _FETCHSTRFVAL
}

static bool _qrybsmatch(const EJQF *qf, const void *bsbuf, int bsbufsz) {
    if (qf->tcop == TDBQTRUE) {
        return !qf->negate;
    }
    bson_iterator it;
    bson_iterator_from_buffer(&it, bsbuf);
    bson_type bt = bson_find_fieldpath_value2(qf->fpath, qf->fpathsz, &it);
    if (bt == BSON_EOO || bt == BSON_UNDEFINED || bt == BSON_NULL) {
        return qf->negate; //Field missing
    } else if (qf->tcop == TDBQCEXIST) {
        return !qf->negate;
    }
    return _qrybsvalmatch(qf, &it, true);
}

static bool _qryormatch(EJCOLL *jcoll,
        EJQ *ejq,
        const void *pkbuf, int pkbufsz,
        const void *bsbuf, int bsbufsz) {
    if (ejq->orqobjsnum <= 0 || !ejq->orqobjs) {
        return true;
    }
    bool rv = false;
    void *nbsbuf = NULL;
    if (bsbuf == NULL) {
        int cbufsz;
        int lbsbufsz;
        char *cbuf = tchdbget(jcoll->tdb->hdb, pkbuf, pkbufsz, &cbufsz);
        if (!cbuf) {
            return false;
        }
        nbsbuf = tcmaploadone(cbuf, cbufsz, JDBCOLBSON, JDBCOLBSONL, &lbsbufsz); //BSON buffer
        if (!nbsbuf) {
            TCFREE(cbuf);
            return false;
        }
        bsbufsz = lbsbufsz;
        bsbuf = nbsbuf;
    }
    if (ejq->lastmatchedorqf && _qrybsmatch(ejq->lastmatchedorqf, bsbuf, bsbufsz)) {
        rv = true;
        goto finish;
    }
    for (int i = 0; i < ejq->orqobjsnum; ++i) {
        const EJQ *oq = (ejq->orqobjs + i);
        assert(oq && oq->qobjlist);
        for (int j = 0; j < TCLISTNUM(oq->qobjlist); ++j) {
            const EJQF *qf = TCLISTVALPTR(oq->qobjlist, j);
            assert(qf);
            if (qf == ejq->lastmatchedorqf) {
                continue;
            }
            if (_qrybsmatch(qf, bsbuf, bsbufsz)) {
                ejq->lastmatchedorqf = qf;
                rv = true;
                goto finish;
            }
        }
    }
finish:
    if (nbsbuf) { //BSON Buffer created by me
        TCFREE(nbsbuf);
    }
    return rv;
}

/** Caller must TCFREE(*bsbuf) if it return TRUE */
static bool _qryallcondsmatch(
        bool onlycount, int anum,
        EJCOLL *jcoll, const EJQF **qfs, int qfsz,
        const void *pkbuf, int pkbufsz,
        void **bsbuf, int *bsbufsz) {
    if (onlycount && anum < 1) {
        *bsbuf = NULL;
        *bsbufsz = 0;
        return true;
    }
    bool rv = true;
    //Load BSON
    int cbufsz;
    char *cbuf = tchdbget(jcoll->tdb->hdb, pkbuf, pkbufsz, &cbufsz);
    if (!cbuf) {
        *bsbuf = NULL;
        *bsbufsz = 0;
        return false;
    }
    *bsbuf = tcmaploadone(cbuf, cbufsz, JDBCOLBSON, JDBCOLBSONL, bsbufsz); //BSON buffer
    if (!*bsbuf) {
        TCFREE(cbuf);
        *bsbufsz = 0;
        return false;
    }
    if (anum < 1) {
        TCFREE(cbuf);
        return true;
    }
    for (int i = 0; i < qfsz; ++i) {
        const EJQF *qf = qfs[i];
        if (qf->flags & EJFEXCLUDED) continue;
        if (!_qrybsmatch(qf, *bsbuf, *bsbufsz)) {
            rv = false;
            break;
        }
    }
    TCFREE(cbuf);
    if (!rv) {
        TCFREE(*bsbuf);
        *bsbuf = NULL;
        *bsbufsz = 0;
    }
    return rv;
}

typedef struct {
    const EJQF **ofs;
    int ofsz;
} _EJBSORTCTX;

/* RS sorting comparison func */
static int _ejdbsoncmp(const TCLISTDATUM *d1, const TCLISTDATUM *d2, void *opaque) {
    _EJBSORTCTX *ctx = opaque;
    assert(ctx);
    int res = 0;
    for (int i = 0; !res && i < ctx->ofsz; ++i) {
        const EJQF *qf = ctx->ofs[i];
        if (qf->flags & EJFORDERUSED) {
            continue;
        }
        res = bson_compare(d1->ptr, d2->ptr, qf->fpath, qf->fpathsz) * (qf->order >= 0 ? 1 : -1);
    }
    return res;
}

EJDB_INLINE void _nufetch(_EJDBNUM *nu, const char *sval, bson_type bt) {
    if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
        nu->inum = tctdbatoi(sval);
    } else if (bt == BSON_DOUBLE) {
        nu->dnum = tctdbatof2(sval);
    } else {
        nu->inum = 0;
        assert(0);
    }
}

EJDB_INLINE int _nucmp(_EJDBNUM *nu, const char *sval, bson_type bt) {
    if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
        int64_t v = tctdbatoi(sval);
        return (nu->inum > v) ? 1 : (nu->inum < v ? -1 : 0);
    } else if (bt == BSON_DOUBLE) {
        double v = tctdbatof2(sval);
        return (nu->dnum > v) ? 1 : (nu->dnum < v ? -1 : 0);
    } else {
        assert(0);
    }
    return 0;
}

EJDB_INLINE int _nucmp2(_EJDBNUM *nu1, _EJDBNUM *nu2, bson_type bt) {
    if (bt == BSON_INT || bt == BSON_LONG || bt == BSON_BOOL) {
        return (nu1->inum > nu2->inum) ? 1 : (nu1->inum < nu2->inum ? -1 : 0);
    } else if (bt == BSON_DOUBLE) {
        return (nu1->dnum > nu2->dnum) ? 1 : (nu1->dnum < nu2->dnum ? -1 : 0);
    } else {
        assert(0);
    }
    return 0;
}

static void _qryfieldup(const EJQF *src, EJQF *target, uint32_t qflags) {
    assert(src && target);
    memset(target, 0, sizeof (*target));
    target->exprdblval = src->exprdblval;
    target->exprlongval = src->exprlongval;
    target->flags = src->flags;
    target->ftype = src->ftype;
    target->negate = src->negate;
    target->order = src->order;
    target->orderseq = src->orderseq;
    target->tcop = src->tcop;
    if (src->expr) {
        TCMEMDUP(target->expr, src->expr, src->exprsz);
        target->exprsz = src->exprsz;
    }
    if (src->fpath) {
        TCMEMDUP(target->fpath, src->fpath, src->fpathsz);
        target->fpathsz = src->fpathsz;
    }
    if (src->regex && (EJQINTERNAL & qflags)) {
        //We cannot do deep copy of regex_t so do shallow copy only for internal query objects
        target->regex = src->regex;
    }
    if (src->exprlist) {
        target->exprlist = tclistdup(src->exprlist);
    }
    if (src->exprmap) {
        target->exprmap = tcmapdup(src->exprmap);
    }
    if (src->updateobj) {
        target->updateobj = bson_dup(src->updateobj);
    }
}

/* Clone query object */
static void _qrydup(const EJQ *src, EJQ *target, uint32_t qflags) {
    assert(src && target);
    memset(target, 0, sizeof (*target));
    target->flags = src->flags | qflags;
    target->max = src->max;
    target->skip = src->skip;
    target->orqobjsnum = src->orqobjsnum;
    target->orqobjs = NULL;

    if (src->qobjlist) {
        target->qobjlist = tclistnew2(TCLISTNUM(src->qobjlist));
        for (int i = 0; i < TCLISTNUM(src->qobjlist); ++i) {
            EJQF qf;
            _qryfieldup(TCLISTVALPTR(src->qobjlist, i), &qf, qflags);
            qf.q = target;
            TCLISTPUSH(target->qobjlist, &qf, sizeof (qf));
        }
    }
    if (src->hints) {
        target->hints = bson_dup(src->hints);
    }
    if (src->orqobjsnum > 0 && src->orqobjs) {
        TCMALLOC(target->orqobjs, sizeof (*(target->orqobjs)) * src->orqobjsnum);
        for (int i = 0; i < src->orqobjsnum; ++i) {
            _qrydup(src->orqobjs + i, target->orqobjs + i, qflags);
        }
    }
}

typedef struct {
    bson *sbson;
    TCMAP *ifields;
    int nstack; //nestet object stack pos
    int matched; //number of matched include fields
} _BSONSTRIPVISITORCTX;

static bson_visitor_cmd_t _bsonstripvisitor(const char *ipath, int ipathlen, const char *key, int keylen,
        const bson_iterator *it, bool after, void *op) {
    _BSONSTRIPVISITORCTX *ictx = op;
    assert(ictx && ictx->sbson && ictx->ifields && ipath && key && it && op);
    bson_visitor_cmd_t rv = BSON_VCMD_OK;
    TCMAP *ifields = ictx->ifields;
    const void *buf;
    const char* ifpath;
    int bufsz;

    bson_type bt = bson_iterator_type(it);
    if (bt == BSON_EOO || (ictx->matched == TCMAPRNUM(ifields))) {
        return BSON_VCMD_TERMINATE;
    }
    if (bt != BSON_OBJECT && bt != BSON_ARRAY) {
        if (after) { //simple primitive case
            return BSON_VCMD_OK;
        }
        //const void *tcmapget(const TCMAP *map, const void *kbuf, int ksiz, int *sp);
        buf = tcmapget(ifields, ipath, ipathlen, &bufsz);
        if (buf) {
            ictx->matched++;
            bson_append_field_from_iterator(it, ictx->sbson);
        }
        return (BSON_VCMD_SKIP_AFTER);
    } else { //more complicated case
        if (!after) {
            buf = tcmapget(ifields, ipath, ipathlen, &bufsz);
            if (buf) { //field hitted
                bson_iterator cit = *it; //copy iterator
                bson_append_field_from_iterator(&cit, ictx->sbson);
                ictx->matched++;
                return (BSON_VCMD_SKIP_NESTED | BSON_VCMD_SKIP_AFTER);
            } else { //check prefix
                tcmapiterinit(ifields);
                //const char *tcmapiternext2(TCMAP *map);
                while ((ifpath = tcmapiternext2(ifields)) != NULL) {
                    int i = 0;
                    for (; i < ipathlen && *(ifpath + i) == *(ipath + i); ++i);
                    if (i == ipathlen) { //ipath prefixes some included field
                        ictx->nstack++;
                        if (bt == BSON_OBJECT) {
                            bson_append_start_object(ictx->sbson, key);
                        } else if (bt == BSON_ARRAY) {
                            bson_append_start_array(ictx->sbson, key);
                        } else {
                            assert(0);
                        }
                        break;
                    }
                }
            }
        } else { //after
            if (ictx->nstack > 0) {
                --ictx->nstack;
                if (bt == BSON_OBJECT) {
                    bson_append_finish_object(ictx->sbson);
                } else if (bt == BSON_ARRAY) {
                    bson_append_finish_array(ictx->sbson);
                } else {
                    assert(0);
                }
            }
        }
    }
    return rv;
}

/* push bson into rs with only fields listed in ifields */
static void _pushstripbson(TCLIST *rs, TCMAP *ifields, void *bsbuf, int bsbufsz) {
    if (!ifields || TCMAPRNUM(ifields) <= 0) {
        tclistpushmalloc(rs, bsbuf, bsbufsz);
        return;
    }
    char bstack[JBSBUFFERSZ];
    void *tmpbuf = (bsbufsz < JBSBUFFERSZ) ? bstack : MYMALLOC(bsbufsz);
    bson sbson;
    char *sdata;
    bson_reset(&sbson);
    sbson.data = tmpbuf;
    sbson.cur = sbson.data + 4;
    sbson.dataSize = MAX(bsbufsz, JBSBUFFERSZ);
    if (sbson.data == bstack) {
        sbson.flags |= BSON_FLAG_STACK_ALLOCATED;
    }

    _BSONSTRIPVISITORCTX ictx;
    ictx.sbson = &sbson;
    ictx.ifields = ifields;
    ictx.nstack = 0;
    ictx.matched = 0;

    //Now copy filtered bson fields
    bson_iterator it;
    bson_iterator_from_buffer(&it, bsbuf);
    bson_visit_fields(&it, BSON_TRAVERSE_ARRAYS_EXCLUDED, _bsonstripvisitor, &ictx);
    assert(ictx.nstack == 0);
    if (bson_finish(&sbson) == BSON_OK) {
        if (sbson.flags & BSON_FLAG_STACK_ALLOCATED) {
            sdata = sbson.data;
            TCLISTPUSH(rs, sbson.data, bson_size(&sbson));
        } else {
            sdata = NULL;
            tclistpushmalloc(rs, sbson.data, bson_size(&sbson));
        }
    } else {
        sdata = sbson.data;
        assert(0);
    }
    //Cleanup
    sbson.data = NULL; //hide allocated data from bson_destroy()
    bson_destroy(&sbson);
    if (sdata && sdata != bstack) {
        TCFREE(sdata);
    }
    TCFREE(bsbuf);
}

static bool _qryupdate(EJCOLL *jcoll, const EJQ *ejq, void *bsbuf, int bsbufsz, TCLIST *didxctx, TCXSTR *log) {
    assert(ejq->flags & EJQUPDATING);
    assert(didxctx);

    bool rv = true;
    bool update = false;

    bson_oid_t *oid;
    bson_type bt, bt2;
    bson_iterator it, it2;
    TCMAP *rowm = NULL;

    if (ejq->flags & EJQDROPALL) { //Record will be dropped
        bt = bson_find_from_buffer(&it, bsbuf, JDBIDKEYNAME);
        if (bt != BSON_OID) {
            _ejdbsetecode(jcoll->jb, JBEQUPDFAILED, __FILE__, __LINE__, __func__);
            return false;
        }
        oid = bson_iterator_oid(&it);
        assert(oid);
        if (log) {
            char xoid[25];
            bson_oid_to_string(oid, xoid);
            tcxstrprintf(log, "$DROPALL ON: %s\n", xoid);
        }
        const void *olddata;
        int olddatasz = 0;
        TCMAP *rmap = tctdbget(jcoll->tdb, oid, sizeof (*oid));
        if (rmap) {
            olddata = tcmapget3(rmap, JDBCOLBSON, JDBCOLBSONL, &olddatasz);
            if (!_updatebsonidx(jcoll, oid, NULL, olddata, olddatasz, didxctx) ||
                    !tctdbout(jcoll->tdb, oid, sizeof (*oid))) {
                rv = false;
            }
            tcmapdel(rmap);
        }
        return rv;
    }

    //Apply update operation
    bson src;
    bson_create_from_buffer2(&src, bsbuf, bsbufsz);

    bson bsout;
    bsout.data = NULL;
    bsout.dataSize = 0;
    bson_reset(&bsout);

    const EJQF *setqf = NULL;
    const EJQF *incqf = NULL;
    const EJQF *addsetqf = NULL;
    const EJQF *pullqf = NULL;

    //$set, $inc, $addToSet, $pull operations
    for (int i = 0; i < TCLISTNUM(ejq->qobjlist); ++i) {
        const EJQF *qf = TCLISTVALPTR(ejq->qobjlist, i);
        if (qf->updateobj == NULL) {
            continue;
        }
        if (!setqf && qf->flags & EJCONDSET) { //$set
            setqf = qf;
        } else if (!incqf && qf->flags & EJCONDINC) {
            incqf = qf;
        } else if (!addsetqf && qf->flags & EJCONDADDSET) {
            addsetqf = qf;
        } else if (!pullqf && qf->flags & EJCONDPULL) {
            pullqf = qf;
        } else {
            assert(0);
            break;
        }
    }
    if (setqf) { //$set
        update = true;
        bson_init_size(&bsout, bsbufsz);
        if (bson_merge(&src, setqf->updateobj, true, &bsout)) {
            rv = false;
            _ejdbsetecode(jcoll->jb, JBEQUPDFAILED, __FILE__, __LINE__, __func__);
        }
        bson_finish(&bsout);
    }
    if (!rv) {
        goto finish;
    }
    if (incqf) { //$inc
        if (!bsout.data) {
            bson_create_from_buffer2(&bsout, bsbuf, bsbufsz);
        }
        bson_iterator_init(&it, incqf->updateobj);
        while ((bt = bson_iterator_next(&it)) != BSON_EOO) {
            if (!BSON_IS_NUM_TYPE(bt)) {
                continue;
            }
            bson_iterator_init(&it2, &bsout);
            bt2 = bson_find_fieldpath_value(bson_iterator_key(&it), &it2);
            if (!BSON_IS_NUM_TYPE(bt2)) {
                continue;
            }
            if (bt2 == BSON_DOUBLE) {
                double v = bson_iterator_double(&it2);
                if (bt == BSON_DOUBLE) {
                    v += bson_iterator_double(&it);
                } else {
                    v += bson_iterator_long(&it);
                }
                if (bson_inplace_set_double(&it2, v)) {
                    rv = false;
                    _ejdbsetecode(jcoll->jb, JBEQUPDFAILED, __FILE__, __LINE__, __func__);
                    break;
                }
                update = true;
            } else {
                int64_t v = bson_iterator_long(&it2);
                v += bson_iterator_long(&it);
                if (bson_inplace_set_long(&it2, v)) {
                    rv = false;
                    _ejdbsetecode(jcoll->jb, JBEQUPDFAILED, __FILE__, __LINE__, __func__);
                    break;
                }
                update = true;
            }
        }
    }
    if (!rv) {
        goto finish;
    }

    if (addsetqf) { //$addToSet
        char* inbuf = (bsout.finished) ? bsout.data : bsbuf;
        if (bson_find_unmerged_array_sets(bson_data(addsetqf->updateobj), inbuf)) {
            //Missing $addToSet element in some array field found
            if (bsout.finished) {
                //reinit `bsout`, `inbuf` already points to `bsout.data` and will be freed later
                bson_init_size(&bsout, bson_size(&bsout));
            } else {
                assert(bsout.data == NULL);
                bson_init_size(&bsout, bsbufsz);
            }
            //$addToSet merge
            if (bson_merge_array_sets(bson_data(addsetqf->updateobj), inbuf, &bsout)) {
                rv = false;
                _ejdbsetecode(jcoll->jb, JBEQUPDFAILED, __FILE__, __LINE__, __func__);
            }
            if (inbuf != bsbuf) {
                TCFREE(inbuf);
            }
            bson_finish(&bsout);
            update = true;
            //bson_print(stderr, &bsout);
        }
    }

    if (!update || !rv) {
        goto finish;
    }
    if (bsout.err) {
        rv = false;
        _ejdbsetecode(jcoll->jb, JBEQUPDFAILED, __FILE__, __LINE__, __func__);
        goto finish;
    }
    //Perform updating
    bt = bson_find(&it, &src, JDBIDKEYNAME);
    if (bt != BSON_OID) {
        rv = false;
        _ejdbsetecode(jcoll->jb, JBEQUPDFAILED, __FILE__, __LINE__, __func__);
        goto finish;
    }
    oid = bson_iterator_oid(&it);
    rowm = tcmapnew2(TCMAPTINYBNUM);
    tcmapput(rowm, JDBCOLBSON, JDBCOLBSONL, bson_data(&bsout), bson_size(&bsout));
    rv = tctdbput(jcoll->tdb, oid, sizeof (*oid), rowm);
    if (rv) {
        rv = _updatebsonidx(jcoll, oid, &bsout, bsbuf, bsbufsz, didxctx);
    }

finish:
    bson_destroy(&src);
    bson_destroy(&bsout);
    if (rowm) {
        tcmapdel(rowm);
    }
    return rv;
}

/** Query */
static TCLIST* _qryexecute(EJCOLL *jcoll, const EJQ *q, uint32_t *outcount, int qflags, TCXSTR *log) {
    //Clone the query object
    EJQ *ejq;
    TCMALLOC(ejq, sizeof (*ejq));
    _qrydup(q, ejq, EJQINTERNAL);

    *outcount = 0;
    bool onlycount = (qflags & JBQRYCOUNT); //quering only for result set count
    bool all = false; //need all records

    EJQF *mqf = NULL; //main indexed query condition if exists
    TCMAP *ifields = NULL; //field names included in result set

    TCLIST *res = onlycount ? NULL : tclistnew2(4096);
    _qrypreprocess(jcoll, ejq, qflags, &mqf, &ifields);

    int anum = 0; //number of active conditions
    const EJQF **ofs = NULL; //order fields
    int ofsz = 0; //order fields count
    int aofsz = 0; //active order fields count
    const EJQF **qfs = NULL; //condition fields array
    const int qfsz = TCLISTNUM(ejq->qobjlist); //number of all condition fields
    if (qfsz > 0) {
        TCMALLOC(qfs, qfsz * sizeof (EJQF*));
    }

    const void *kbuf;
    int kbufsz;
    const void *vbuf;
    int vbufsz;
    void *bsbuf; //BSON buffer
    int bsbufsz; //BSON buffer size
    static const bool yes = true; //Simple true const

    uint32_t count = 0; //current count
    uint32_t max = (ejq->max > 0) ? ejq->max : UINT_MAX;
    uint32_t skip = ejq->skip;
    const TDBIDX *midx = mqf ? mqf->idx : NULL;
    TCHDB *hdb = jcoll->tdb->hdb;
    assert(hdb);

    TCLIST *didxctx = NULL; //deffered indexing context
    if (ejq->flags & EJQUPDATING) {
        didxctx = tclistnew();
    }

    if (midx) { //Main index used for ordering
        if (mqf->orderseq == 1 &&
                !(mqf->tcop == TDBQCSTRAND || mqf->tcop == TDBQCSTROR || mqf->tcop == TDBQCSTRNUMOR)) {
            mqf->flags |= EJFORDERUSED;
        }
    }
    for (int i = 0; i < qfsz; ++i) {
        EJQF *qf = TCLISTVALPTR(ejq->qobjlist, i);
        assert(qf);
        if (log && qf->exprmap) {
            tcxstrprintf(log, "USING HASH TOKENS IN: %s\n", qf->fpath);
        }
        qf->jb = jcoll->jb;
        qfs[i] = qf;
        if (qf->fpathsz > 0 && !(qf->flags & EJFEXCLUDED)) {
            anum++;
        }
        if (qf->orderseq) {
            ofsz++;
            if (onlycount) {
                qf->flags |= EJFORDERUSED;
            }
        }
    }
    if (ofsz > 0) { //Collect order fields array
        TCMALLOC(ofs, ofsz * sizeof (EJQF*));
        for (int i = 0; i < ofsz; ++i) {
            for (int j = 0; j < qfsz; ++j) {
                if (qfs[j]->orderseq == i + 1) { //orderseq starts with 1
                    ofs[i] = qfs[j];
                    if (!(ofs[i]->flags & EJFORDERUSED)) {
                        aofsz++;
                        if (ifields) { //add this order field to the list of fetched
                            tcmapputkeep(ifields, ofs[i]->fpath, ofs[i]->fpathsz, &yes, sizeof (yes));
                        }
                    }
                    break;
                }
            }
        }
        for (int i = 0; i < ofsz; ++i) assert(ofs[i] != NULL);
    }

    if (!onlycount && aofsz > 0 && (!midx || mqf->orderseq != 1)) { //Main index is not the main order field
        all = true; //Need all records for ordering for some other fields
    }

    if (log) {
        tcxstrprintf(log, "UPDATING MODE: %s\n", (ejq->flags & EJQUPDATING) ? "YES" : "NO");
        tcxstrprintf(log, "MAX: %u\n", max);
        tcxstrprintf(log, "SKIP: %u\n", skip);
        tcxstrprintf(log, "COUNT ONLY: %s\n", onlycount ? "YES" : "NO");
        tcxstrprintf(log, "MAIN IDX: '%s'\n", midx ? midx->name : "NONE");
        tcxstrprintf(log, "ORDER FIELDS: %d\n", ofsz);
        tcxstrprintf(log, "ACTIVE CONDITIONS: %d\n", anum);
        tcxstrprintf(log, "$OR QUERIES: %d\n", ejq->orqobjsnum);
        tcxstrprintf(log, "FETCH ALL: %s\n", all ? "YES" : "NO");
    }
    if (max < UINT_MAX - skip) {
        max += skip;
    }
    if (max == 0) {
        goto finish;
    }
    if (!midx && (!mqf || !(mqf->flags & EJFPKMATCHING))) { //Missing main index & no PK matching
        goto fullscan;
    }
    if (log) {
        tcxstrprintf(log, "MAIN IDX TCOP: %d\n", mqf->tcop);
    }

#define JBQREGREC(_bsbuf, _bsbufsz)   \
    ++count; \
    if (ejq->flags & EJQUPDATING) { \
        _qryupdate(jcoll, ejq, (_bsbuf), (_bsbufsz), didxctx, log); \
    } \
    if (!onlycount && (all || count > skip)) { \
        if (ifields) {\
            _pushstripbson(res, ifields, (_bsbuf), (_bsbufsz)); \
        } else { \
            tclistpushmalloc(res, (_bsbuf), _bsbufsz); \
        } \
    } else if ((_bsbuf)) { \
        TCFREE((_bsbuf)); \
    }

    bool trim = (midx && *midx->name != '\0');
    if (anum > 0 && !(mqf->flags & EJFEXCLUDED)) {
        anum--;
        mqf->flags |= EJFEXCLUDED;
    }

    if (mqf->flags & EJFPKMATCHING) { //PK matching
        if (log) {
            tcxstrprintf(log, "PRIMARY KEY MATCHING: TRUE\n");
        }
        assert(mqf->expr);
        if (mqf->tcop == TDBQCSTREQ) {
            do {
                bson_oid_t oid;
                bson_oid_from_string(&oid, mqf->expr);
                void *cdata = tchdbget(jcoll->tdb->hdb, &oid, sizeof (oid), &bsbufsz);
                if (!cdata) {
                    break;
                }
                bsbuf = tcmaploadone(cdata, bsbufsz, JDBCOLBSON, JDBCOLBSONL, &bsbufsz);
                if (!bsbuf) {
                    TCFREE(cdata);
                    break;
                }
                bool matched = true;
                for (int i = 0; i < qfsz; ++i) {
                    const EJQF *qf = qfs[i];
                    if (qf->flags & EJFEXCLUDED) continue;
                    if (!_qrybsmatch(qf, bsbuf, bsbufsz)) {
                        matched = false;
                        break;
                    }
                }
                if (matched && (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, &oid, sizeof (oid), bsbuf, bsbufsz))) {
                    JBQREGREC(bsbuf, bsbufsz);
                } else {
                    TCFREE(bsbuf);
                }
                TCFREE(cdata);
            } while (false);
        } else if (mqf->tcop == TDBQCSTROREQ) {
            TCLIST *tokens = mqf->exprlist;
            assert(tokens);
            tclistsort(tokens);
            for (int i = 1; i < TCLISTNUM(tokens); i++) {
                if (!strcmp(TCLISTVALPTR(tokens, i), TCLISTVALPTR(tokens, i - 1))) {
                    TCFREE(tclistremove2(tokens, i));
                    i--;
                }
            }
            int tnum = TCLISTNUM(tokens);
            for (int i = 0; (all || count < max) && i < tnum; i++) {
                const char *token;
                int tsiz;
                TCLISTVAL(token, tokens, i, tsiz);
                if (tsiz < 1) {
                    continue;
                }
                bson_oid_t oid;
                bson_oid_from_string(&oid, token);
                void *cdata = tchdbget(jcoll->tdb->hdb, &oid, sizeof (oid), &bsbufsz);
                if (!cdata) {
                    continue;
                }
                bsbuf = tcmaploadone(cdata, bsbufsz, JDBCOLBSON, JDBCOLBSONL, &bsbufsz);
                if (!bsbuf) {
                    TCFREE(cdata);
                    break;
                }
                bool matched = true;
                for (int i = 0; i < qfsz; ++i) {
                    const EJQF *qf = qfs[i];
                    if (qf->flags & EJFEXCLUDED) continue;
                    if (!_qrybsmatch(qf, bsbuf, bsbufsz)) {
                        matched = false;
                        break;
                    }
                }
                if (matched && (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, &oid, sizeof (oid), bsbuf, bsbufsz))) {
                    JBQREGREC(bsbuf, bsbufsz);
                } else {
                    TCFREE(bsbuf);
                }
                TCFREE(cdata);
            }
        } else {
            assert(0);
        }
    } else if (mqf->tcop == TDBQTRUE) {
        BDBCUR *cur = tcbdbcurnew(midx->db);
        if (mqf->order >= 0) {
            tcbdbcurfirst(cur);
        } else {
            tcbdbcurlast(cur);
        }
        while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
            if (trim) kbufsz -= 3;
            vbuf = tcbdbcurval3(cur, &vbufsz);
            if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz)) {
                JBQREGREC(bsbuf, bsbufsz);
            }
            if (mqf->order >= 0) {
                tcbdbcurnext(cur);
            } else {
                tcbdbcurprev(cur);
            }
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCSTREQ) { /* string is equal to */
        assert(midx->type == TDBITLEXICAL);
        char *expr = mqf->expr;
        int exprsz = mqf->exprsz;
        BDBCUR *cur = tcbdbcurnew(midx->db);
        tcbdbcurjump(cur, expr, exprsz + trim);
        while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
            if (trim) kbufsz -= 3;
            if (kbufsz == exprsz && !memcmp(kbuf, expr, exprsz)) {
                vbuf = tcbdbcurval3(cur, &vbufsz);
                if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                        (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                    JBQREGREC(bsbuf, bsbufsz);
                }
            } else {
                break;
            }
            tcbdbcurnext(cur);
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCSTRBW) { /* string begins with */
        assert(midx->type == TDBITLEXICAL);
        char *expr = mqf->expr;
        int exprsz = mqf->exprsz;
        BDBCUR *cur = tcbdbcurnew(midx->db);
        tcbdbcurjump(cur, expr, exprsz + trim);
        while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
            if (trim) kbufsz -= 3;
            if (kbufsz >= exprsz && !memcmp(kbuf, expr, exprsz)) {
                vbuf = tcbdbcurval3(cur, &vbufsz);
                if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                        (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                    JBQREGREC(bsbuf, bsbufsz);
                }
            } else {
                break;
            }
            tcbdbcurnext(cur);
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCSTRORBW) { /* string begins with one token in */
        assert(mqf->ftype == BSON_ARRAY);
        assert(midx->type == TDBITLEXICAL);
        BDBCUR *cur = tcbdbcurnew(midx->db);
        TCLIST *tokens = mqf->exprlist;
        assert(tokens);
        tclistsort(tokens);
        for (int i = 1; i < TCLISTNUM(tokens); i++) {
            if (!strcmp(TCLISTVALPTR(tokens, i), TCLISTVALPTR(tokens, i - 1))) {
                TCFREE(tclistremove2(tokens, i));
                i--;
            }
        }
        if (mqf->order < 0 && (mqf->flags & EJFORDERUSED)) {
            tclistinvert(tokens);
        }
        int tnum = TCLISTNUM(tokens);
        for (int i = 0; (all || count < max) && i < tnum; i++) {
            const char *token;
            int tsiz;
            TCLISTVAL(token, tokens, i, tsiz);
            if (tsiz < 1) continue;
            tcbdbcurjump(cur, token, tsiz + trim);
            while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
                if (trim) kbufsz -= 3;
                if (kbufsz >= tsiz && !memcmp(kbuf, token, tsiz)) {
                    vbuf = tcbdbcurval3(cur, &vbufsz);
                    if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                            (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                        JBQREGREC(bsbuf, bsbufsz);
                    }
                } else {
                    break;
                }
                tcbdbcurnext(cur);
            }
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCSTROREQ) { /* string is equal to at least one token in */
        assert(mqf->ftype == BSON_ARRAY);
        assert(midx->type == TDBITLEXICAL);
        BDBCUR *cur = tcbdbcurnew(midx->db);
        TCLIST *tokens = mqf->exprlist;
        assert(tokens);
        tclistsort(tokens);
        for (int i = 1; i < TCLISTNUM(tokens); i++) {
            if (!strcmp(TCLISTVALPTR(tokens, i), TCLISTVALPTR(tokens, i - 1))) {
                TCFREE(tclistremove2(tokens, i));
                i--;
            }
        }
        if (mqf->order < 0 && (mqf->flags & EJFORDERUSED)) {
            tclistinvert(tokens);
        }
        int tnum = TCLISTNUM(tokens);
        for (int i = 0; (all || count < max) && i < tnum; i++) {
            const char *token;
            int tsiz;
            TCLISTVAL(token, tokens, i, tsiz);
            if (tsiz < 1) continue;
            tcbdbcurjump(cur, token, tsiz + trim);
            while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
                if (trim) kbufsz -= 3;
                if (kbufsz == tsiz && !memcmp(kbuf, token, tsiz)) {
                    vbuf = tcbdbcurval3(cur, &vbufsz);
                    if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                            (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                        JBQREGREC(bsbuf, bsbufsz);
                    }
                } else {
                    break;
                }
                tcbdbcurnext(cur);
            }
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCNUMEQ) { /* number is equal to */
        assert(midx->type == TDBITDECIMAL);
        char *expr = mqf->expr;
        int exprsz = mqf->exprsz;
        BDBCUR *cur = tcbdbcurnew(midx->db);
        _EJDBNUM num;
        _nufetch(&num, expr, mqf->ftype);
        tctdbqryidxcurjumpnum(cur, expr, exprsz, true);
        while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
            if (_nucmp(&num, kbuf, mqf->ftype) == 0) {
                vbuf = tcbdbcurval3(cur, &vbufsz);
                if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                        (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                    JBQREGREC(bsbuf, bsbufsz);
                }
            } else {
                break;
            }
            tcbdbcurnext(cur);
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCNUMGT || mqf->tcop == TDBQCNUMGE) {
        /* number is greater than | number is greater than or equal to */
        assert(midx->type == TDBITDECIMAL);
        char *expr = mqf->expr;
        int exprsz = mqf->exprsz;
        BDBCUR *cur = tcbdbcurnew(midx->db);
        _EJDBNUM xnum;
        _nufetch(&xnum, expr, mqf->ftype);
        if (mqf->order < 0 && (mqf->flags & EJFORDERUSED)) { //DESC
            tcbdbcurlast(cur);
            while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
                _EJDBNUM knum;
                _nufetch(&knum, kbuf, mqf->ftype);
                int cmp = _nucmp2(&knum, &xnum, mqf->ftype);
                if (cmp < 0) break;
                if (cmp > 0 || (mqf->tcop == TDBQCNUMGE && cmp >= 0)) {
                    vbuf = tcbdbcurval3(cur, &vbufsz);
                    if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                            (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                        JBQREGREC(bsbuf, bsbufsz);
                    }
                }
                tcbdbcurprev(cur);
            }
        } else { //ASC
            tctdbqryidxcurjumpnum(cur, expr, exprsz, true);
            while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
                _EJDBNUM knum;
                _nufetch(&knum, kbuf, mqf->ftype);
                int cmp = _nucmp2(&knum, &xnum, mqf->ftype);
                if (cmp > 0 || (mqf->tcop == TDBQCNUMGE && cmp >= 0)) {
                    vbuf = tcbdbcurval3(cur, &vbufsz);
                    if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                            (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                        JBQREGREC(bsbuf, bsbufsz);
                    }
                }
                tcbdbcurnext(cur);
            }
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCNUMLT || mqf->tcop == TDBQCNUMLE) {
        /* number is less than | number is less than or equal to */
        assert(midx->type == TDBITDECIMAL);
        char *expr = mqf->expr;
        int exprsz = mqf->exprsz;
        BDBCUR *cur = tcbdbcurnew(midx->db);
        _EJDBNUM xnum;
        _nufetch(&xnum, expr, mqf->ftype);
        if (mqf->order >= 0) { //ASC
            tcbdbcurfirst(cur);
            while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
                _EJDBNUM knum;
                _nufetch(&knum, kbuf, mqf->ftype);
                int cmp = _nucmp2(&knum, &xnum, mqf->ftype);
                if (cmp > 0) break;
                if (cmp < 0 || (cmp <= 0 && mqf->tcop == TDBQCNUMLE)) {
                    vbuf = tcbdbcurval3(cur, &vbufsz);
                    if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                            (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                        JBQREGREC(bsbuf, bsbufsz);
                    }
                }
                tcbdbcurnext(cur);
            }
        } else {
            tctdbqryidxcurjumpnum(cur, expr, exprsz, false);
            while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
                _EJDBNUM knum;
                _nufetch(&knum, kbuf, mqf->ftype);
                int cmp = _nucmp2(&knum, &xnum, mqf->ftype);
                if (cmp < 0 || (cmp <= 0 && mqf->tcop == TDBQCNUMLE)) {
                    vbuf = tcbdbcurval3(cur, &vbufsz);
                    if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                            (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                        JBQREGREC(bsbuf, bsbufsz);
                    }
                }
                tcbdbcurprev(cur);
            }
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCNUMBT) { /* number is between two tokens of */
        assert(mqf->ftype == BSON_ARRAY);
        assert(midx->type == TDBITDECIMAL);
        assert(mqf->exprlist);
        TCLIST *tokens = mqf->exprlist;
        assert(TCLISTNUM(tokens) == 2);
        const char *expr;
        int exprsz;
        long double lower = tctdbatof(tclistval2(tokens, 0));
        long double upper = tctdbatof(tclistval2(tokens, 1));
        expr = tclistval2(tokens, (lower > upper) ? 1 : 0);
        exprsz = strlen(expr);
        if (lower > upper) {
            long double swap = lower;
            lower = upper;
            upper = swap;
        }
        BDBCUR *cur = tcbdbcurnew(midx->db);
        tctdbqryidxcurjumpnum(cur, expr, exprsz, true);
        while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
            if (tctdbatof(kbuf) > upper) break;
            vbuf = tcbdbcurval3(cur, &vbufsz);
            if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                    (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                JBQREGREC(bsbuf, bsbufsz);
            }
            tcbdbcurnext(cur);
        }
        tcbdbcurdel(cur);
        if (!all && !onlycount && mqf->order < 0 && (mqf->flags & EJFORDERUSED)) { //DESC
            tclistinvert(res);
        }
    } else if (mqf->tcop == TDBQCNUMOREQ) { /* number is equal to at least one token in */
        assert(mqf->ftype == BSON_ARRAY);
        assert(midx->type == TDBITDECIMAL);
        BDBCUR *cur = tcbdbcurnew(midx->db);
        TCLIST *tokens = mqf->exprlist;
        assert(tokens);
        tclistsortex(tokens, tdbcmppkeynumasc);
        for (int i = 1; i < TCLISTNUM(tokens); i++) {
            if (tctdbatof(TCLISTVALPTR(tokens, i)) == tctdbatof(TCLISTVALPTR(tokens, i - 1))) {
                TCFREE(tclistremove2(tokens, i));
                i--;
            }
        }
        if (mqf->order < 0 && (mqf->flags & EJFORDERUSED)) {
            tclistinvert(tokens);
        }
        int tnum = TCLISTNUM(tokens);
        for (int i = 0; (all || count < max) && i < tnum; i++) {
            const char *token;
            int tsiz;
            TCLISTVAL(token, tokens, i, tsiz);
            if (tsiz < 1) continue;
            long double xnum = tctdbatof(token);
            tctdbqryidxcurjumpnum(cur, token, tsiz, true);
            while ((all || count < max) && (kbuf = tcbdbcurkey3(cur, &kbufsz)) != NULL) {
                if (tctdbatof(kbuf) == xnum) {
                    vbuf = tcbdbcurval3(cur, &vbufsz);
                    if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, vbuf, vbufsz, &bsbuf, &bsbufsz) &&
                            (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, vbuf, vbufsz, bsbuf, bsbufsz))) {
                        JBQREGREC(bsbuf, bsbufsz);
                    }
                } else {
                    break;
                }
                tcbdbcurnext(cur);
            }
        }
        tcbdbcurdel(cur);
    } else if (mqf->tcop == TDBQCSTRAND || mqf->tcop == TDBQCSTROR || mqf->tcop == TDBQCSTRNUMOR) {
        /* string includes all tokens in | string includes at least one token in */
        assert(midx->type == TDBITTOKEN);
        assert(mqf->ftype == BSON_ARRAY);
        TCLIST *tokens = mqf->exprlist;
        assert(tokens);
        if (mqf->tcop == TDBQCSTRNUMOR) {
            tclistsortex(tokens, tdbcmppkeynumasc);
            for (int i = 1; i < TCLISTNUM(tokens); i++) {
                if (tctdbatof(TCLISTVALPTR(tokens, i)) == tctdbatof(TCLISTVALPTR(tokens, i - 1))) {
                    TCFREE(tclistremove2(tokens, i));
                    i--;
                }
            }
        } else {
            tclistsort(tokens);
            for (int i = 1; i < TCLISTNUM(tokens); i++) {
                if (!strcmp(TCLISTVALPTR(tokens, i), TCLISTVALPTR(tokens, i - 1))) {
                    TCFREE(tclistremove2(tokens, i));
                    i--;
                }
            }
        }
        TCMAP *tres = tctdbidxgetbytokens(jcoll->tdb, midx, tokens, mqf->tcop, log);
        tcmapiterinit(tres);
        while ((all || count < max) && (kbuf = tcmapiternext(tres, &kbufsz)) != NULL) {
            if (_qryallcondsmatch(onlycount, anum, jcoll, qfs, qfsz, kbuf, kbufsz, &bsbuf, &bsbufsz) &&
                    (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, kbuf, kbufsz, bsbuf, bsbufsz))) {
                JBQREGREC(bsbuf, bsbufsz);
            }
        }
        tcmapdel(tres);
    }

    if (onlycount) {
        goto finish;
    } else {
        goto sorting;
    }

fullscan: /* Full scan */
    assert(count == 0);
    assert(!res || TCLISTNUM(res) == 0);
    if (log) {
        tcxstrprintf(log, "RUN FULLSCAN\n");
    }

    uint64_t hdbiter;
    TCMAP *updkeys = (ejq->flags & EJQUPDATING) ? tcmapnew2(100 * 1024) : NULL;
    if (!tchdbiterinit4(hdb, &hdbiter)) {
        goto finish;
    }
    TCXSTR *skbuf = tcxstrnew3(sizeof (bson_oid_t) + 1);
    TCXSTR *scbuf = tcxstrnew3(1024);
    int rows = 0;
    while ((all || count < max) && tchdbiternext4(hdb, &hdbiter, skbuf, scbuf)) {
        ++rows;
        void *bsbuf = tcmaploadone(TCXSTRPTR(scbuf), TCXSTRSIZE(scbuf), JDBCOLBSON, JDBCOLBSONL, &bsbufsz);
        if (!bsbuf) {
            goto wfinish;
        }
        bool matched = true;
        for (int i = 0; i < qfsz; ++i) {
            const EJQF *qf = qfs[i];
            if (qf->flags & EJFEXCLUDED) {
                continue;
            }
            if (!_qrybsmatch(qf, bsbuf, bsbufsz)) {
                matched = false;
                break;
            }
        }
        if (matched && (ejq->orqobjsnum == 0 || _qryormatch(jcoll, ejq, TCXSTRPTR(skbuf), TCXSTRSIZE(skbuf), bsbuf, bsbufsz))) {
            if (updkeys) { //we are in updating mode
                if (tcmapputkeep(updkeys, TCXSTRPTR(skbuf), TCXSTRSIZE(skbuf), &yes, sizeof (yes))) {
                    JBQREGREC(bsbuf, bsbufsz);
                } else {
                    TCFREE(bsbuf);
                }
            } else {
                JBQREGREC(bsbuf, bsbufsz);
            }
        } else {
            TCFREE(bsbuf);
        }
wfinish:
        tcxstrclear(skbuf);
        tcxstrclear(scbuf);
    }
    if (updkeys) {
        tcmapdel(updkeys);
    }
    tcxstrdel(skbuf);
    tcxstrdel(scbuf);

sorting: /* Sorting resultset */
    if (!res || aofsz <= 0) { //No sorting needed
        goto finish;
    }
    _EJBSORTCTX sctx; //sorting context
    sctx.ofs = ofs;
    sctx.ofsz = ofsz;
    if (ejdbtimsortlist(res, _ejdbsoncmp, &sctx)) {
        _ejdbsetecode(jcoll->jb, JBEQRSSORTING, __FILE__, __LINE__, __func__);
    }
finish:
    //revert max
    if (max < UINT_MAX && max > skip) {
        max = max - skip;
    }
    if (res) {
        if (all) { //skipping results after full sorting with skip > 0
            for (int i = 0; i < skip && res->num > 0; ++i) {
                TCFREE(res->array[res->start].ptr);
                ++(res->start);
                --(res->num);
            }
        }
        if ((res->start & 0xff) == 0 && res->start > (res->num >> 1)) {
            memmove(res->array, res->array + res->start, res->num * sizeof (res->array[0]));
            res->start = 0;
        }
        if (TCLISTNUM(res) > max) { //truncate results if max specified
            int end = res->start + res->num;
            TCLISTDATUM *array = res->array;
            for (int i = (res->start + max); i < end; i++) {
                TCFREE(array[i].ptr);
                --(res->num);
            }
        }
    }
    count = (skip < count) ? count - skip : 0;
    if (count > max) {
        count = max;
    }
    *outcount = count;
    if (log) {
        tcxstrprintf(log, "RS COUNT: %d\n", count);
        tcxstrprintf(log, "RS SIZE: %d\n", (res ? TCLISTNUM(res) : 0));
        tcxstrprintf(log, "FINAL SORTING: %s\n", (onlycount || aofsz <= 0) ? "NO" : "YES");
    }

    //Apply deffered index changes
    if (didxctx) {
        for (int i = TCLISTNUM(didxctx) - 1; i >= 0; --i) {
            _DEFFEREDIDXCTX *di = TCLISTVALPTR(didxctx, i);
            assert(di);
            if (di->rmap) {
                tctdbidxout2(jcoll->tdb, &(di->oid), sizeof (di->oid), di->rmap);
                tcmapdel(di->rmap);
            }
            if (di->imap) {
                tctdbidxput2(jcoll->tdb, &(di->oid), sizeof (di->oid), di->imap);
                tcmapdel(di->imap);
            }
        }
        tclistdel(didxctx);
    }

    //Cleanup
    if (qfs) {
        TCFREE(qfs);
    }
    if (ofs) {
        TCFREE(ofs);
    }
    if (ifields) {
        tcmapdel(ifields);
    }
    ejdbquerydel(ejq);
#undef JBQREGREC
    return res;
}

static TDBIDX* _qryfindidx(EJCOLL *jcoll, EJQF *qf, bson *idxmeta) {
    TCTDB *tdb = jcoll->tdb;
    char p = '\0';
    switch (qf->tcop) {
        case TDBQCSTREQ:
        case TDBQCSTRBW:
        case TDBQCSTROREQ:
        case TDBQCSTRORBW:
            p = (qf->flags & EJCONDICASE) ? 'i' : 's'; //lexical string index
            break;
        case TDBQCNUMEQ:
        case TDBQCNUMGT:
        case TDBQCNUMGE:
        case TDBQCNUMLT:
        case TDBQCNUMLE:
        case TDBQCNUMBT:
        case TDBQCNUMOREQ:
            p = 'n'; //number index
            break;
        case TDBQCSTRAND:
        case TDBQCSTROR:
            p = 'a'; //token index
            break;
        case TDBQTRUE:
            p = 'o'; //take first appropriate index
            break;
    }
    if (p == '\0' || !qf->fpath || !qf->fpathsz) {
        return NULL;
    }
    for (int i = 0; i < tdb->inum; ++i) {
        TDBIDX *idx = tdb->idxs + i;
        assert(idx);
        if (p == 'o') {
            if (*idx->name == 'a' || *idx->name == 'i') { //token index or icase index not the best solution here
                continue;
            }
        } else if (*idx->name != p) {
            continue;
        }
        if (!strcmp(qf->fpath, idx->name + 1)) {
            return idx;
        }
    }
    //No direct operation index. Any alternatives?
    if (idxmeta &&
            !(qf->flags & EJCONDICASE) && //if not case insensitive query
            (qf->tcop == TDBQCSTREQ || qf->tcop == TDBQCSTROREQ || qf->tcop == TDBQCNUMOREQ)) {
        bson_iterator it;
        bson_type bt = bson_find(&it, idxmeta, "iflags");
        if (bt != BSON_INT) {
            return NULL;
        }
        int iflags = bson_iterator_int(&it);
        if (iflags & JBIDXARR) { //array token index exists so convert qf into TDBQCSTROR
            for (int i = 0; i < tdb->inum; ++i) {
                TDBIDX *idx = tdb->idxs + i;
                if (!strcmp(qf->fpath, idx->name + 1)) {
                    if (qf->tcop == TDBQCSTREQ) {
                        qf->tcop = TDBQCSTROR;
                        qf->exprlist = tclistnew2(1);
                        TCLISTPUSH(qf->exprlist, qf->expr, qf->exprsz);
                        if (qf->expr) TCFREE(qf->expr);
                        qf->expr = tclistdump(qf->exprlist, &qf->exprsz);
                        qf->ftype = BSON_ARRAY;
                        return idx;
                    } else if (qf->tcop == TDBQCSTROREQ) {
                        assert(qf->ftype == BSON_ARRAY);
                        qf->tcop = TDBQCSTROR;
                        return idx;
                    } else if (qf->tcop == TDBQCNUMOREQ) {
                        assert(qf->ftype == BSON_ARRAY);
                        qf->tcop = TDBQCSTRNUMOR;
                        return idx;
                    }
                }
            }
        }
    }
    return NULL;
}

static void _qrypreprocess(EJCOLL *jcoll, EJQ *ejq, int qflags, EJQF **mqf, TCMAP **ifields) {
    assert(jcoll && ejq && ejq->qobjlist && mqf);

    *ifields = NULL;
    *mqf = NULL;

    EJQF *oqf = NULL; //Order condition
    TCLIST *qlist = ejq->qobjlist;
    bson_iterator it;
    bson_type bt;

    if (ejq->hints) {
        bson_type bt;
        bson_iterator it, sit;
        //Process $orderby
        bt = bson_find(&it, ejq->hints, "$orderby");
        if (bt == BSON_OBJECT) {
            int orderseq = 1;
            bson_iterator_subiterator(&it, &sit);
            while ((bt = bson_iterator_next(&sit)) != BSON_EOO) {
                if (!BSON_IS_NUM_TYPE(bt)) {
                    continue;
                }
                const char *ofield = bson_iterator_key(&sit);
                int odir = bson_iterator_int(&sit);
                odir = (odir > 0) ? 1 : (odir < 0 ? -1 : 0);
                if (!odir) {
                    continue;
                }
                EJQF nqf;
                EJQF *qf = NULL;
                for (int i = 0; i < TCLISTNUM(qlist); ++i) {
                    if (!strcmp(ofield, ((EJQF*) TCLISTVALPTR(qlist, i))->fpath)) {
                        qf = TCLISTVALPTR(qlist, i);
                        assert(qf);
                        break;
                    }
                }
                if (qf == NULL) { //Create syntetic query field for orderby ops
                    memset(&nqf, 0, sizeof (EJQF));
                    nqf.fpath = tcstrdup(ofield);
                    nqf.fpathsz = strlen(nqf.fpath);
                    nqf.expr = tcstrdup("");
                    nqf.exprsz = 0;
                    nqf.tcop = TDBQTRUE; //disable any TC matching operation
                    nqf.ftype = BSON_OBJECT;
                    nqf.orderseq = orderseq++;
                    nqf.order = odir;
                    nqf.flags |= EJFEXCLUDED; //field excluded  from matching
                    qf = &nqf;
                    TCLISTPUSH(qlist, qf, sizeof (*qf));
                } else {
                    qf->orderseq = orderseq++;
                    qf->order = odir;
                }
            }
        }
        bt = bson_find(&it, ejq->hints, "$skip");
        if (BSON_IS_NUM_TYPE(bt)) {
            int64_t v = bson_iterator_long(&it);
            ejq->skip = (uint32_t) ((v < 0) ? 0 : v);
        }
        bt = bson_find(&it, ejq->hints, "$max");
        if (BSON_IS_NUM_TYPE(bt)) {
            int64_t v = bson_iterator_long(&it);
            ejq->max = (uint32_t) ((v < 0) ? 0 : v);
        }
        if (!(qflags & JBQRYCOUNT)) {
            bt = bson_find(&it, ejq->hints, "$fields"); //Collect required fields
            if (bt == BSON_OBJECT) {
                TCMAP *fmap = tcmapnew2(TCMAPTINYBNUM);
                static const bool yes = true;
                bson_iterator_subiterator(&it, &sit);
                while ((bt = bson_iterator_next(&sit)) != BSON_EOO) {
                    if (!BSON_IS_NUM_TYPE(bt) || bson_iterator_int(&sit) <= 0) {
                        continue;
                    }
                    const char *key = bson_iterator_key(&sit);
                    tcmapputkeep(fmap, key, strlen(key), &yes, sizeof (yes));
                }
                tcmapputkeep(fmap, JDBIDKEYNAME, JDBIDKEYNAMEL, &yes, sizeof (yes));
                *ifields = fmap;
            }
        }
    }

    const int scoreexact = 100;
    const int scoregtlt = 50;
    int maxiscore = 0; //Maximum index score
    int maxselectivity = 0;

    for (int i = 0; i < TCLISTNUM(qlist); ++i) {
        int iscore = 0;
        EJQF *qf = (EJQF*) TCLISTVALPTR(qlist, i);
        assert(qf && qf->fpath);
        if (qf->flags & (EJCONDSET | EJCONDINC)) { //skip updating qfields
            continue;
        }
        //OID PK matching
        if (!qf->negate && (qf->tcop == TDBQCSTREQ || qf->tcop == TDBQCSTROREQ) && !strcmp(JDBIDKEYNAME, qf->fpath)) {
            qf->flags |= EJFPKMATCHING;
            *mqf = qf;
            break;
        }

        bool firstorderqf = false;
        qf->idxmeta = _imetaidx(jcoll, qf->fpath);
        qf->idx = _qryfindidx(jcoll, qf, qf->idxmeta);
        if (qf->order && qf->orderseq == 1) { //Index for first 'orderby' exists
            oqf = qf;
            firstorderqf = true;
        }
        if (!qf->idx || !qf->idxmeta) {
            qf->idx = NULL;
            qf->idxmeta = NULL;
            continue;
        }
        if (qf->tcop == TDBQTRUE || qf->negate) {
            continue;
        }
        int avgreclen = -1;
        int selectivity = -1;
        bt = bson_find(&it, qf->idxmeta, "selectivity");
        if (bt == BSON_DOUBLE) {
            selectivity = (int) ((double) bson_iterator_double(&it) * 100); //Selectivity percent
        }
        bt = bson_find(&it, qf->idxmeta, "avgreclen");
        if (bt == BSON_DOUBLE) {
            avgreclen = (int) bson_iterator_double(&it);
        }
        if (selectivity > 0) {
            if (selectivity <= 20) { //Not using index at all if selectivity lesser than 20%
                continue;
            }
            iscore += selectivity;
        }
        if (firstorderqf) {
            iscore += (maxselectivity - selectivity) / 2;
        }
        if (selectivity > maxselectivity) {
            maxselectivity = selectivity;
        }
        switch (qf->tcop) {
            case TDBQCSTREQ:
            case TDBQCSTROR:
            case TDBQCNUMEQ:
            case TDBQCNUMBT:
                iscore += scoreexact;
                break;
            case TDBQCSTRBW:
            case TDBQCSTREW:
                if (avgreclen > 0 && qf->exprsz > avgreclen) {
                    iscore += scoreexact;
                }
                break;
            case TDBQCNUMGT:
            case TDBQCNUMGE:
            case TDBQCNUMLT:
            case TDBQCNUMLE:
                if (firstorderqf) {
                    iscore += scoreexact;
                } else {
                    iscore += scoregtlt;
                }
                break;
        }
        if (iscore >= maxiscore) {
            *mqf = qf;
            maxiscore = iscore;
        }
    }
    if (*mqf == NULL && (oqf && oqf->idx && !oqf->negate)) {
        *mqf = oqf;
    }
}

static bool _metasetopts(EJDB *jb, const char *colname, EJCOLLOPTS *opts) {
    bool rv = true;
    if (!opts) {
        return _metasetbson(jb, colname, strlen(colname), "opts", NULL, false, false);
    }
    bson *bsopts = bson_create();
    bson_init(bsopts);
    bson_append_bool(bsopts, "compressed", opts->compressed);
    bson_append_bool(bsopts, "large", opts->large);
    bson_append_int(bsopts, "cachedrecords", opts->cachedrecords);
    bson_append_int(bsopts, "records", opts->records);
    bson_finish(bsopts);
    rv = _metasetbson(jb, colname, strlen(colname), "opts", bsopts, false, false);
    bson_del(bsopts);
    return rv;
}

static bool _metagetopts(EJDB *jb, const char *colname, EJCOLLOPTS *opts) {
    assert(opts);
    bool rv = true;
    memset(opts, 0, sizeof (*opts));
    bson *bsopts = _metagetbson(jb, colname, strlen(colname), "opts");
    if (!bsopts) {
        return true;
    }
    bson_iterator it;
    bson_type bt = bson_find(&it, bsopts, "compressed");
    if (bt == BSON_BOOL) {
        opts->compressed = bson_iterator_bool(&it);
    }
    bt = bson_find(&it, bsopts, "large");
    if (bt == BSON_BOOL) {
        opts->large = bson_iterator_bool(&it);
    }
    bt = bson_find(&it, bsopts, "cachedrecords");
    if (BSON_IS_NUM_TYPE(bt)) {
        opts->cachedrecords = bson_iterator_long(&it);
    }
    bt = bson_find(&it, bsopts, "records");
    if (BSON_IS_NUM_TYPE(bt)) {
        opts->records = bson_iterator_long(&it);
    }
    return rv;
}

static bool _metasetbson(EJDB *jb, const char *colname, int colnamesz,
        const char *mkey, bson *val, bool merge, bool mergeoverwrt) {
    assert(jb && colname && mkey);
    bool rv = true;
    bson *bsave = NULL;
    bson *oldval = NULL;
    bson mresult;

    TCMAP *cmeta = tctdbget(jb->metadb, colname, colnamesz);
    if (!cmeta) {
        _ejdbsetecode(jb, JBEMETANVALID, __FILE__, __LINE__, __func__);
        rv = false;
        goto finish;
    }
    if (!val) {
        if (tcmapout2(cmeta, mkey)) {
            rv = tctdbput(jb->metadb, colname, colnamesz, cmeta);
        }
        goto finish;
    }
    assert(val);
    if (merge) { //Merged
        oldval = _metagetbson(jb, colname, colnamesz, mkey);
        if (oldval) {
            bson_init(&mresult);
            bson_merge(oldval, val, mergeoverwrt, &mresult);
            bson_finish(&mresult);
            bsave = &mresult;
        } else {
            bsave = val;
        }
    } else { //Rewrited
        bsave = val;
    }

    assert(bsave);
    tcmapput(cmeta, mkey, strlen(mkey), bson_data(bsave), bson_size(bsave));
    rv = tctdbput(jb->metadb, colname, colnamesz, cmeta);
finish:
    if (oldval) {
        if (merge) {
            bson_destroy(bsave);
        }
        bson_del(oldval);
    }
    if (cmeta) {
        tcmapdel(cmeta);
    }
    tctdbsync(jb->metadb);
    return rv;
}

static bool _metasetbson2(EJCOLL *jcoll, const char *mkey, bson *val, bool merge, bool mergeoverwrt) {
    assert(jcoll);
    return _metasetbson(jcoll->jb, jcoll->cname, jcoll->cnamesz, mkey, val, merge, mergeoverwrt);
}

/**Returned meta BSON data must be freed by 'bson_del' */
static bson* _metagetbson(EJDB *jb, const char *colname, int colnamesz, const char *mkey) {
    assert(jb && colname && mkey);
    bson *rv = NULL;
    TCMAP *cmeta = tctdbget(jb->metadb, colname, colnamesz);
    if (!cmeta) {
        _ejdbsetecode(jb, JBEMETANVALID, __FILE__, __LINE__, __func__);
        return NULL;
    }
    int bsz;
    const void *raw = tcmapget(cmeta, mkey, strlen(mkey), &bsz);
    if (!raw || bsz == 0) {
        goto finish;
    }
    rv = bson_create();
    bson_init_size(rv, bsz);
    bson_ensure_space(rv, bsz - 4);
    bson_append(rv, ((char*) raw) + 4, bsz - (4 + 1/*BSON_EOO*/));
    bson_finish(rv);
finish:
    tcmapdel(cmeta);
    return rv;
}

static bson* _metagetbson2(EJCOLL *jcoll, const char *mkey) {
    assert(jcoll);
    return _metagetbson(jcoll->jb, jcoll->cname, jcoll->cnamesz, mkey);
}

/**Returned index meta if not NULL it must be freed by 'bson_del' */
static bson* _imetaidx(EJCOLL *jcoll, const char *ipath) {
    assert(jcoll && ipath);
    if (*ipath == '\0') {
        return NULL;
    }
    bson *rv = NULL;
    char fpathkey[BSON_MAX_FPATH_LEN + 1];
    TCMAP *cmeta = tctdbget(jcoll->jb->metadb, jcoll->cname, jcoll->cnamesz);
    if (!cmeta) {
        _ejdbsetecode(jcoll->jb, JBEMETANVALID, __FILE__, __LINE__, __func__);
        goto finish;
    }
    int klen = snprintf(fpathkey, BSON_MAX_FPATH_LEN + 1, "i%s", ipath); //'i' prefix for all columns with index meta
    if (klen > BSON_MAX_FPATH_LEN) {
        _ejdbsetecode(jcoll->jb, JBEFPATHINVALID, __FILE__, __LINE__, __func__);
        goto finish;
    }
    int bsz;
    const void *bsdata = tcmapget(cmeta, fpathkey, klen, &bsz);
    if (bsdata) {
        rv = bson_create();
        bson_init_size(rv, bsz);
        bson_ensure_space(rv, bsz - 4);
        bson_append(rv, ((char*) bsdata) + 4, bsz - (4 + 1));
        bson_finish(rv);
    }
finish:
    if (cmeta) {
        tcmapdel(cmeta);
    }
    return rv;
}

/** Free EJQF field **/
static void _delqfdata(const EJQ *q, const EJQF *qf) {
    assert(q && qf);
    if (qf->expr) {
        TCFREE(qf->expr);
    }
    if (qf->fpath) {
        TCFREE(qf->fpath);
    }
    if (qf->idxmeta) {
        bson_del(qf->idxmeta);
    }
    if (qf->updateobj) {
        bson_del(qf->updateobj);
    }
    if (qf->regex && !(EJQINTERNAL & q->flags)) {
        //We do not clear regex_t data because it not deep copy in internal queries
        regfree(qf->regex);
        TCFREE(qf->regex);
    }
    if (qf->exprlist) {
        tclistdel(qf->exprlist);
    }
    if (qf->exprmap) {
        tcmapdel(qf->exprmap);
    }
}

/**
 * Copy BSON array into new TCLIST. TCLIST must be freed by 'tclistdel'.
 * @param it BSON iterator
 * @param type[out] Detected BSON type of last element
 */
static TCLIST* _fetch_bson_str_array(EJDB *jb, bson_iterator *it, bson_type *type, txtflags_t tflags) {
    TCLIST *res = tclistnew();
    *type = BSON_EOO;
    bson_type ftype;
    for (int i = 0; (ftype = bson_iterator_next(it)) != BSON_EOO; ++i) {
        switch (ftype) {
            case BSON_STRING:
                *type = ftype;
                if (tflags & JBICASE) { //ignore case
                    char *buf = NULL;
                    char sbuf[JBSTRINOPBUFFERSZ];
                    int len = tcicaseformat(bson_iterator_string(it), bson_iterator_string_len(it) - 1, sbuf, JBSTRINOPBUFFERSZ, &buf);
                    if (len < 0) {
                        _ejdbsetecode(jb, len, __FILE__, __LINE__, __func__);
                        break;
                    }
                    tclistpush2(res, buf);
                    if (buf && buf != sbuf) {
                        TCFREE(buf);
                    }
                } else {
                    tclistpush2(res, bson_iterator_string(it));
                }
                break;
            case BSON_INT:
            case BSON_LONG:
            case BSON_BOOL:
                *type = ftype;
                tclistprintf(res, "%ld", bson_iterator_long(it));
                break;
            case BSON_DOUBLE:
                *type = ftype;
                tclistprintf(res, "%f", bson_iterator_double(it));
                break;
            case BSON_OID:
                *type = ftype;
                char xoid[25];
                bson_oid_to_string(bson_iterator_oid(it), xoid);
                tclistprintf(res, "%s", xoid);
                break;
            default:
                break;
        }
    }
    return res;
}

/** result must be freed by TCFREE */
static char* _fetch_bson_str_array2(EJDB *jb, bson_iterator *it, bson_type *type, txtflags_t tflags) {
    TCLIST *res = _fetch_bson_str_array(jb, it, type, tflags);
    char *tokens = tcstrjoin(res, ',');
    tclistdel(res);
    return tokens;
}

static int _parse_qobj_impl(EJDB *jb, EJQ *q, bson_iterator *it, TCLIST *qlist, TCLIST *pathStack, EJQF *pqf) {
    assert(it && qlist && pathStack);
    int ret = 0;
    bson_type ftype;
    bool yes = true;
    while ((ftype = bson_iterator_next(it)) != BSON_EOO) {

        const char *fkey = bson_iterator_key(it);
        bool isckey = (*fkey == '$'); //Key is a control key: $in, $nin, $not, $all

        if (!isckey && pqf && pqf->ftype == BSON_ARRAY) {
            //All index keys in array are prefixed with '*'. Eg: 'store.*1.item', 'store.*2.item', ...
            char akey[TCNUMBUFSIZ];
            assert(strlen(fkey) <= TCNUMBUFSIZ - 2);
            akey[0] = '*';
            memmove(akey + 1, fkey, strlen(fkey) + 1);
            fkey = akey;
        }

        EJQF qf;
        memset(&qf, 0, sizeof (qf));
        qf.q = q;

        if (!isckey) {
            //Push key on top of path stack
            tclistpush2(pathStack, fkey);
            qf.ftype = ftype;
        } else {
            if (!strcmp("$set", fkey) ||
                    !strcmp("$inc", fkey) ||
                    !strcmp("$dropall", fkey) ||
                    !strcmp("$addToSet", fkey)) {
                if (pqf) { //Top level ops
                    ret = JBEQERROR;
                    break;
                }
            } else {
                if (!pqf) { //Require parent query object
                    ret = JBEQERROR;
                    break;
                }
                qf = *pqf;
                if (!strcmp("$not", fkey)) {
                    qf.negate = !qf.negate;
                } else if (!strcmp("$gt", fkey)) {
                    qf.flags |= EJCOMPGT;
                } else if (!strcmp("$gte", fkey)) {
                    qf.flags |= EJCOMPGTE;
                } else if (!strcmp("$lt", fkey)) {
                    qf.flags |= EJCOMPLT;
                } else if (!strcmp("$lte", fkey)) {
                    qf.flags |= EJCOMPLTE;
                } else if (!strcmp("$begin", fkey)) {
                    qf.flags |= EJCONDSTARTWITH;
                } else if (!strcmp("$icase", fkey)) {
                    qf.flags |= EJCONDICASE;
                }
            }
        }

        switch (ftype) {
            case BSON_ARRAY:
            {
                if (isckey) {
                    bson_iterator sit;
                    bson_iterator_subiterator(it, &sit);
                    bson_type atype = 0;
                    TCLIST *tokens = _fetch_bson_str_array(jb, &sit, &atype, (qf.flags & EJCONDICASE) ? JBICASE : 0);
                    if (atype == 0) {
                        ret = JBEQINOPNOTARRAY;
                        tclistdel(tokens);
                        break;
                    }
                    assert(!qf.expr && !qf.fpath);
                    qf.ftype = BSON_ARRAY;
                    qf.expr = tclistdump(tokens, &qf.exprsz);
                    qf.exprlist = tokens;
                    if (!strcmp("$in", fkey) || !strcmp("$nin", fkey)) {
                        if (!strcmp("$nin", fkey)) {
                            qf.negate = true;
                        }
                        if (BSON_IS_NUM_TYPE(atype) || atype == BSON_BOOL) {
                            qf.tcop = TDBQCNUMOREQ;
                        } else {
                            qf.tcop = TDBQCSTROREQ;
                            if (TCLISTNUM(tokens) >= JBINOPTMAPTHRESHOLD) {
                                assert(!qf.exprmap);
                                qf.exprmap = tcmapnew2(TCLISTNUM(tokens));
                                for (int i = 0; i < TCLISTNUM(tokens); ++i) {
                                    tcmapputkeep(qf.exprmap, TCLISTVALPTR(tokens, i), TCLISTVALSIZ(tokens, i), &yes, sizeof (yes));
                                }
                            }
                        }
                    } else if (!strcmp("$bt", fkey)) { //between
                        qf.tcop = TDBQCNUMBT;
                        if (TCLISTNUM(tokens) != 2) {
                            ret = JBEQINOPNOTARRAY;
                            TCFREE(qf.expr);
                            tclistdel(qf.exprlist);
                            break;
                        }
                    } else if (!strcmp("$strand", fkey)) { //$strand
                        qf.tcop = TDBQCSTRAND;
                    } else if (!strcmp("$stror", fkey)) { //$stror
                        qf.tcop = TDBQCSTROR;
                    } else if (qf.flags & EJCONDSTARTWITH) { //$begin with some token
                        qf.tcop = TDBQCSTRORBW;
                    } else {
                        ret = JBEQINVALIDQCONTROL;
                        TCFREE(qf.expr);
                        tclistdel(qf.exprlist);
                        break;
                    }
                    qf.fpath = tcstrjoin(pathStack, '.');
                    qf.fpathsz = strlen(qf.fpath);
                    TCLISTPUSH(qlist, &qf, sizeof (qf));
                    break;
                } else {
                    bson_iterator sit;
                    bson_iterator_subiterator(it, &sit);
                    ret = _parse_qobj_impl(jb, q, &sit, qlist, pathStack, &qf);
                    break;
                }
            }

            case BSON_OBJECT:
            {
                if (isckey) {
                    if (!pqf && !strcmp("$set", fkey)) { //top level set OP
                        qf.flags |= EJCONDSET;
                    } else if (!pqf && !strcmp("$addToSet", fkey)) {
                        qf.flags |= EJCONDADDSET;
                    } else if (!strcmp("$inc", fkey)) {
                        qf.flags |= EJCONDINC;
                    }
                    if ((qf.flags & (EJCONDSET | EJCONDINC | EJCONDADDSET))) {
                        assert(qf.updateobj == NULL);
                        qf.q->flags |= EJQUPDATING;
                        qf.updateobj = bson_create();
                        bson_init_as_query(qf.updateobj);
                        bson_type sbt;
                        bson_iterator sit;
                        bson_iterator_subiterator(it, &sit);
                        while ((sbt = bson_iterator_next(&sit)) != BSON_EOO) {
                            bson_append_field_from_iterator(&sit, qf.updateobj);
                        }
                        bson_finish(qf.updateobj);
                        if (qf.updateobj->err) {
                            ret = JBEQERROR;
                        }
                        qf.fpath = strdup(fkey);
                        qf.fpathsz = strlen(qf.fpath);
                        qf.tcop = TDBQTRUE;
                        qf.flags |= EJFEXCLUDED;
                        TCLISTPUSH(qlist, &qf, sizeof (qf));
                        break;
                    }
                }
                bson_iterator sit;
                bson_iterator_subiterator(it, &sit);
                ret = _parse_qobj_impl(jb, q, &sit, qlist, pathStack, &qf);
                break;
            }
            case BSON_OID:
            {
                assert(!qf.fpath && !qf.expr);
                qf.ftype = ftype;
                TCMALLOC(qf.expr, 25 * sizeof (char));
                bson_oid_to_string(bson_iterator_oid(it), qf.expr);
                qf.exprsz = 24;
                qf.fpath = tcstrjoin(pathStack, '.');
                qf.fpathsz = strlen(qf.fpath);
                qf.tcop = TDBQCSTREQ;
                TCLISTPUSH(qlist, &qf, sizeof (qf));
                break;
            }
            case BSON_STRING:
            {
                assert(!qf.fpath && !qf.expr);
                qf.ftype = ftype;
                if (qf.flags & EJCONDICASE) {
                    qf.exprsz = tcicaseformat(bson_iterator_string(it), bson_iterator_string_len(it) - 1, NULL, 0, &qf.expr);
                    if (qf.exprsz < 0) {
                        ret = qf.exprsz;
                        qf.exprsz = 0;
                        break;
                    }
                } else {
                    qf.expr = tcstrdup(bson_iterator_string(it));
                    qf.exprsz = strlen(qf.expr);
                }

                qf.fpath = tcstrjoin(pathStack, '.');
                qf.fpathsz = strlen(qf.fpath);
                if (qf.flags & EJCONDSTARTWITH) {
                    qf.tcop = TDBQCSTRBW;
                } else {
                    qf.tcop = TDBQCSTREQ;
                    if (!strcmp(JDBIDKEYNAME, fkey)) { //_id
                        qf.ftype = BSON_OID;
                    }
                }
                TCLISTPUSH(qlist, &qf, sizeof (qf));
                break;
            }
            case BSON_LONG:
            case BSON_DOUBLE:
            case BSON_INT:
            {
                assert(!qf.fpath && !qf.expr);
                qf.ftype = ftype;
                qf.fpath = tcstrjoin(pathStack, '.');
                qf.fpathsz = strlen(qf.fpath);
                if (ftype == BSON_LONG || ftype == BSON_INT) {
                    qf.exprlongval = bson_iterator_long(it);
                    qf.exprdblval = qf.exprlongval;
                    qf.expr = tcsprintf("%ld", qf.exprlongval);
                } else {
                    qf.exprdblval = bson_iterator_double(it);
                    qf.exprlongval = (int64_t) qf.exprdblval;
                    qf.expr = tcsprintf("%f", qf.exprdblval);
                }
                qf.exprsz = strlen(qf.expr);
                if (qf.flags & EJCOMPGT) {
                    qf.tcop = TDBQCNUMGT;
                } else if (qf.flags & EJCOMPGTE) {
                    qf.tcop = TDBQCNUMGE;
                } else if (qf.flags & EJCOMPLT) {
                    qf.tcop = TDBQCNUMLT;
                } else if (qf.flags & EJCOMPLTE) {
                    qf.tcop = TDBQCNUMLE;
                } else {
                    qf.tcop = TDBQCNUMEQ;
                }
                TCLISTPUSH(qlist, &qf, sizeof (qf));
                break;
            }
            case BSON_REGEX:
            {
                assert(!qf.fpath && !qf.expr);
                qf.ftype = ftype;
                qf.tcop = TDBQCSTRRX;
                char *re = tcstrdup(bson_iterator_regex(it));
                const char *opts = bson_iterator_regex_opts(it);
                qf.fpath = tcstrjoin(pathStack, '.');
                qf.fpathsz = strlen(qf.fpath);
                qf.expr = re;
                qf.exprsz = strlen(qf.expr);
                const char *rxstr = qf.expr;
                regex_t rxbuf;
                int rxopt = REG_EXTENDED | REG_NOSUB;
                if (strchr(opts, 'i')) {
                    rxopt |= REG_ICASE;
                }
                if (regcomp(&rxbuf, rxstr, rxopt) == 0) {
                    TCMALLOC(qf.regex, sizeof (rxbuf));
                    memcpy(qf.regex, &rxbuf, sizeof (rxbuf));
                } else {
                    ret = JBEQINVALIDQRX;
                    TCFREE(qf.fpath);
                    TCFREE(qf.expr);
                    break;
                }
                TCLISTPUSH(qlist, &qf, sizeof (qf));
                break;
            }
            case BSON_NULL:
            case BSON_UNDEFINED:
                qf.ftype = ftype;
                qf.tcop = TDBQCEXIST;
                qf.negate = !qf.negate;
                qf.expr = tcstrdup(""); //Empty string as expr
                qf.exprsz = 0;
                TCLISTPUSH(qlist, &qf, sizeof (qf));
                break;

            case BSON_BOOL: //boolean converted into number
            {
                bool bv = bson_iterator_bool_raw(it);
                if (isckey) {
                    if (!strcmp("$dropall", fkey) && bv) {
                        qf.flags |= EJFEXCLUDED;
                        qf.fpath = tcstrjoin(pathStack, '.');
                        qf.fpathsz = strlen(qf.fpath);
                        qf.tcop = TDBQTRUE;
                        qf.q->flags |= EJQUPDATING;
                        qf.q->flags |= EJQDROPALL;
                        qf.expr = tcstrdup(""); //Empty string as expr
                        qf.exprsz = 0;
                        TCLISTPUSH(qlist, &qf, sizeof (qf));
                        break;
                    }
                    if (!strcmp("$exists", fkey)) {
                        qf.tcop = TDBQCEXIST;
                        qf.fpath = tcstrjoin(pathStack, '.');
                        qf.fpathsz = strlen(qf.fpath);
                        qf.expr = tcstrdup(""); //Empty string as expr
                        qf.exprsz = 0;
                        if (!bv) {
                            qf.negate = !qf.negate;
                        }
                        TCLISTPUSH(qlist, &qf, sizeof (qf));
                        break;
                    }
                }
                qf.tcop = TDBQCNUMEQ;
                qf.fpath = tcstrjoin(pathStack, '.');
                qf.fpathsz = strlen(qf.fpath);
                qf.exprlongval = (bv ? 1 : 0);
                qf.exprdblval = qf.exprlongval;
                qf.expr = strdup(bv ? "1" : "0");
                qf.exprsz = 1;
                TCLISTPUSH(qlist, &qf, sizeof (qf));
                break;
            }
            default:
                break;
        };

        if (!isckey) {
            assert(pathStack->num > 0);
            TCFREE(tclistpop2(pathStack));
        }
        if (ret) { //cleanup on error condition
            //TODO better error reporting
            _ejdbsetecode(jb, ret, __FILE__, __LINE__, __func__);
            break;
        }
    }

    return ret;
}

/**
 * Convert bson query spec into field path -> EJQF instance.
 *  Created map instance must be freed `tcmapdel`.
 *  Each element of map must be freed by TODO
 */
static TCLIST* _parseqobj(EJDB *jb, EJQ *q, bson *qspec) {
    assert(qspec);
    int rv = 0;
    TCLIST *res = tclistnew2(TCLISTINYNUM);
    bson_iterator it;
    bson_iterator_init(&it, qspec);
    TCLIST *pathStack = tclistnew2(TCLISTINYNUM);
    rv = _parse_qobj_impl(jb, q, &it, res, pathStack, NULL);
    if (rv) {
        tclistdel(res);
        res = NULL;
    }
    assert(!pathStack->num);
    tclistdel(pathStack);
    return res;
}

/**
 * Get OID value from the '_id' field of specified bson object.
 * @param bson[in] BSON object
 * @param oid[out] Pointer to OID type
 * @return True if OID value is found int _id field of bson object otherwise False.
 */
static bson_type _bsonoidkey(bson *bs, bson_oid_t *oid) {
    bson_iterator it;
    bson_type bt = bson_find(&it, bs, JDBIDKEYNAME);
    if (bt == BSON_OID) {
        *oid = *bson_iterator_oid(&it);
    }
    return bt;
}

/**
 * Return string value representation of value pointed by 'it'.
 * Resulting value size stored into 'vsz'.
 * If returned value is not NULL it must be freed by TCFREE.
 */
static char* _bsonitstrval(EJDB *jb, bson_iterator *it, int *vsz, TCLIST *tokens, txtflags_t tflags) {
    int retlen = 0;
    char *ret = NULL;
    bson_type btype = bson_iterator_type(it);
    if (btype == BSON_STRING) {
        if (tokens) { //split string into tokens and push it into 'tokens' list
            const unsigned char *sp = (unsigned char *) bson_iterator_string(it);
            while (*sp != '\0') {
                while ((*sp != '\0' && *sp <= ' ') || *sp == ',') {
                    sp++;
                }
                const unsigned char *ep = sp;
                while (*ep > ' ' && *ep != ',') {
                    ep++;
                }
                if (ep > sp) {
                    if (tflags & JBICASE) { //ignore case mode
                        char *buf = NULL;
                        char sbuf[JBSTRINOPBUFFERSZ];
                        int len = tcicaseformat((const char*) sp, ep - sp, sbuf, JBSTRINOPBUFFERSZ, &buf);
                        if (len >= 0) { //success
                            TCLISTPUSH(tokens, buf, len);
                        } else {
                            _ejdbsetecode(jb, len, __FILE__, __LINE__, __func__);
                        }
                        if (buf && buf != sbuf) {
                            TCFREE(buf);
                        }
                    } else {
                        TCLISTPUSH(tokens, sp, ep - sp);
                    }
                }
                sp = ep;
            }
        } else {
            retlen = bson_iterator_string_len(it) - 1;
            if (tflags & JBICASE) {
                retlen = tcicaseformat(bson_iterator_string(it), retlen, NULL, 0, &ret);
            } else {
                ret = tcmemdup(bson_iterator_string(it), retlen);
            }
        }
    } else if (BSON_IS_NUM_TYPE(btype) || btype == BSON_BOOL) {
        char nbuff[TCNUMBUFSIZ];
        if (btype == BSON_INT || btype == BSON_LONG || btype == BSON_BOOL) {
            retlen = bson_numstrn(nbuff, TCNUMBUFSIZ, bson_iterator_long(it));
            if (retlen >= TCNUMBUFSIZ) {
                retlen = TCNUMBUFSIZ - 1;
            }
        } else if (btype == BSON_DOUBLE) {
            retlen = tcftoa(bson_iterator_double(it), nbuff, TCNUMBUFSIZ, 6);
            if (retlen >= TCNUMBUFSIZ) {
                retlen = TCNUMBUFSIZ - 1;
            }
        }
        if (tflags & JBICASE) {
            retlen = tcicaseformat(nbuff, retlen, NULL, 0, &ret);
        } else {
            ret = tcmemdup(nbuff, retlen);
        }
    } else if (btype == BSON_ARRAY) {
        bson_type eltype; //last element bson type
        bson_iterator sit;
        bson_iterator_subiterator(it, &sit);
        if (tokens) {
            while ((eltype = bson_iterator_next(&sit)) != BSON_EOO) {
                int vz = 0;
                char *v = _bsonitstrval(jb, &sit, &vz, NULL, tflags);
                if (v) {
                    TCLISTPUSH(tokens, v, vz);
                    TCFREE(v);
                }
            }
        } else {
            //Array elements are joined with ',' delimeter.
            ret = _fetch_bson_str_array2(jb, &sit, &eltype, tflags);
            retlen = strlen(ret);
        }
    }
    if (retlen < 0) {
        _ejdbsetecode(jb, retlen, __FILE__, __LINE__, __func__);
        ret = NULL;
        retlen = 0;
    }
    *vsz = retlen;
    return ret;
}

static char* _bsonipathrowldr(
        TCLIST *tokens,
        const char *pkbuf, int pksz,
        const char *rowdata, int rowdatasz,
        const char *ipath, int ipathsz, void *op, int *vsz) {
    assert(op);
    char *res = NULL;
    if (ipath && *ipath == '\0') { //PK TODO review
        if (tokens) {
            const unsigned char *sp = (unsigned char *) pkbuf;
            while (*sp != '\0') {
                while ((*sp != '\0' && *sp <= ' ') || *sp == ',') {
                    sp++;
                }
                const unsigned char *ep = sp;
                while (*ep > ' ' && *ep != ',') {
                    ep++;
                }
                if (ep > sp) TCLISTPUSH(tokens, sp, ep - sp);
                sp = ep;
            }
            *vsz = 0;
            return NULL;
        } else {
            TCMEMDUP(res, pkbuf, pksz);
            *vsz = pksz;
            return res;
        }
    }
    if (!ipath || ipathsz < 2 || *(ipath + 1) == '\0' || strchr("snai", *ipath) == NULL) {
        return NULL;
    }
    //skip index type prefix char with (fpath + 1)
    res = _bsonfpathrowldr(tokens, rowdata, rowdatasz, ipath + 1, ipathsz - 1, op, vsz);
    if (*vsz == 0) { //Do not allow empty strings for index opration
        if (res) TCFREE(res);
        res = NULL;
    }
    return res;
}

static char* _bsonfpathrowldr(TCLIST *tokens, const char *rowdata, int rowdatasz,
        const char *fpath, int fpathsz, void *op, int *vsz) {
    _BSONIPATHROWLDR *odata = (_BSONIPATHROWLDR*) op;
    assert(odata && odata->jcoll);
    char *ret = NULL;
    int bsize;
    bson_iterator it;
    char *bsdata = tcmaploadone(rowdata, rowdatasz, JDBCOLBSON, JDBCOLBSONL, &bsize);
    if (!bsdata) {
        *vsz = 0;
        return NULL;
    }
    bson_iterator_from_buffer(&it, bsdata);
    bson_find_fieldpath_value2(fpath, fpathsz, &it);
    ret = _bsonitstrval(odata->jcoll->jb, &it, vsz, tokens, (odata->icase ? JBICASE : 0));
    TCFREE(bsdata);
    return ret;
}

static bool _updatebsonidx(EJCOLL *jcoll, const bson_oid_t *oid, const bson *bs,
        const void *obsdata, int obsdatasz, TCLIST *dlist) {
    bool rv = true;
    TCMAP *cmeta = tctdbget(jcoll->jb->metadb, jcoll->cname, jcoll->cnamesz);
    if (!cmeta) {
        _ejdbsetecode(jcoll->jb, JBEMETANVALID, __FILE__, __LINE__, __func__);
        return false;
    }
    TCMAP *imap = NULL; //New index map
    TCMAP *rimap = NULL; //Remove index map
    bson_type mt = BSON_EOO;
    bson_type ft = BSON_EOO;
    bson_type oft = BSON_EOO;
    bson_iterator fit, oit, mit;
    int bsz;
    char ikey[BSON_MAX_FPATH_LEN + 2];
    const char *mkey;
    int mkeysz;

    tcmapiterinit(cmeta);
    while ((mkey = tcmapiternext(cmeta, &mkeysz)) != NULL && mkeysz > 0) {
        if (*mkey != 'i' || mkeysz > BSON_MAX_FPATH_LEN + 1) {
            continue;
        }
        const void *mraw = tcmapget(cmeta, mkey, mkeysz, &bsz);
        if (!mraw || !bsz || (mt = bson_find_from_buffer(&mit, mraw, "iflags")) != BSON_INT) {
            continue;
        }
        int iflags = bson_iterator_int(&mit);
        //OK then process index keys
        memcpy(ikey + 1, mkey + 1, mkeysz - 1);
        ikey[mkeysz] = '\0';

        int fvaluesz = 0;
        char *fvalue = NULL;
        int ofvaluesz = 0;
        char *ofvalue = NULL;
        txtflags_t textflags = (iflags & JBIDXISTR) ? JBICASE : 0;

        if (obsdata && obsdatasz > 0) {
            bson_iterator_from_buffer(&oit, obsdata);
            oft = bson_find_fieldpath_value2(mkey + 1, mkeysz - 1, &oit);
            TCLIST *tokens = (oft == BSON_ARRAY) ? tclistnew() : NULL;
            ofvalue = BSON_IS_IDXSUPPORTED_TYPE(oft) ? _bsonitstrval(jcoll->jb, &oit, &ofvaluesz, tokens, textflags) : NULL;
            if (tokens) {
                ofvalue = tclistdump(tokens, &ofvaluesz);
                tclistdel(tokens);
            }
        }
        if (bs) {
            bson_iterator_init(&fit, bs);
            ft = bson_find_fieldpath_value2(mkey + 1, mkeysz - 1, &fit);
            TCLIST *tokens = (ft == BSON_ARRAY) ? tclistnew() : NULL;
            fvalue = BSON_IS_IDXSUPPORTED_TYPE(ft) ? _bsonitstrval(jcoll->jb, &fit, &fvaluesz, tokens, textflags) : NULL;
            if (tokens) {
                fvalue = tclistdump(tokens, &fvaluesz);
                tclistdel(tokens);
            }
        }
        if (!fvalue && !ofvalue) {
            continue;
        }
        if (imap == NULL) {
            imap = tcmapnew2(TCMAPTINYBNUM);
            rimap = tcmapnew2(TCMAPTINYBNUM);
        }
        for (int i = 4; i <= 7; ++i) { /* JBIDXNUM, JBIDXSTR, JBIDXARR, JBIDXISTR */
            bool rm = false;
            int itype = (1 << i);
            if (itype == JBIDXNUM && (JBIDXNUM & iflags)) {
                ikey[0] = 'n';
            } else if (itype == JBIDXSTR && (JBIDXSTR & iflags)) {
                ikey[0] = 's';
            } else if (itype == JBIDXISTR && (JBIDXISTR & iflags)) {
                ikey[0] = 'i';
            } else if (itype == JBIDXARR && (JBIDXARR & iflags)) {
                ikey[0] = 'a';
                if (ofvalue && oft == BSON_ARRAY &&
                        (!fvalue || ft != oft || fvaluesz != ofvaluesz || memcmp(fvalue, ofvalue, fvaluesz))) {
                    tcmapput(rimap, ikey, mkeysz, ofvalue, ofvaluesz);
                    rm = true;
                }
                if (fvalue && fvaluesz > 0 && ft == BSON_ARRAY && (!ofvalue || rm)) {
                    tcmapput(imap, ikey, mkeysz, fvalue, fvaluesz);
                }
                continue;
            } else {
                continue;
            }
            if (ofvalue && oft != BSON_ARRAY &&
                    (!fvalue || ft != oft || fvaluesz != ofvaluesz || memcmp(fvalue, ofvalue, fvaluesz))) {
                tcmapput(rimap, ikey, mkeysz, ofvalue, ofvaluesz);
                rm = true;
            }
            if (fvalue && fvaluesz > 0 && ft != BSON_ARRAY && (!ofvalue || rm)) {
                tcmapput(imap, ikey, mkeysz, fvalue, fvaluesz);
            }
        }
        if (fvalue) TCFREE(fvalue);
        if (ofvalue) TCFREE(ofvalue);
    }
    tcmapdel(cmeta);

    if (dlist) { //storage for deffered index ops provided, save changes into
        _DEFFEREDIDXCTX dctx;
        dctx.oid = *oid;
        dctx.rmap = (rimap && TCMAPRNUM(rimap) > 0) ? tcmapdup(rimap) : NULL;
        dctx.imap = (imap && TCMAPRNUM(imap) > 0) ? tcmapdup(imap) : NULL;
        if (dctx.imap || dctx.rmap) {
            TCLISTPUSH(dlist, &dctx, sizeof (dctx));
        }
    } else { //apply index changes immediately
        if (rimap && !tctdbidxout2(jcoll->tdb, oid, sizeof (*oid), rimap)) rv = false;
        if (imap && !tctdbidxput2(jcoll->tdb, oid, sizeof (*oid), imap)) rv = false;
    }
    if (imap) tcmapdel(imap);
    if (rimap) tcmapdel(rimap);
    return rv;
}

static void _delcoldb(EJCOLL *jcoll) {
    assert(jcoll);
    tctdbdel(jcoll->tdb);
    jcoll->tdb = NULL;
    jcoll->jb = NULL;
    jcoll->cnamesz = 0;
    TCFREE(jcoll->cname);
    if (jcoll->mmtx) {
        pthread_rwlock_destroy(jcoll->mmtx);
        TCFREE(jcoll->mmtx);
    }
}

static bool _addcoldb0(const char *cname, EJDB *jb, EJCOLLOPTS *opts, EJCOLL **res) {
    bool rv = true;
    TCTDB *cdb;
    rv = _createcoldb(cname, jb, opts, &cdb);
    if (!rv) {
        *res = NULL;
        return rv;
    }
    TCREALLOC(jb->cdbs, jb->cdbs, sizeof (jb->cdbs[0]) * (++jb->cdbsnum));
    EJCOLL *jcoll = jb->cdbs + jb->cdbsnum - 1;
    jcoll->cname = tcstrdup(cname);
    jcoll->cnamesz = strlen(cname);
    jcoll->tdb = cdb;
    jcoll->jb = jb;
    jcoll->mmtx = NULL;
    _ejdbcolsetmutex(jcoll);
    *res = jcoll;
    return rv;
}

static bool _createcoldb(const char *colname, EJDB *jb, EJCOLLOPTS *opts, TCTDB **res) {
    assert(jb && jb->metadb);
    if (!JBISVALCOLNAME(colname)) {
        _ejdbsetecode(jb, JBEINVALIDCOLNAME, __FILE__, __LINE__, __func__);
        *res = NULL;
        return false;
    }
    bool rv = true;
    TCTDB *cdb = tctdbnew();
    tctdbsetmutex(cdb);
    if (opts) {
        if (opts->cachedrecords > 0) {
            tctdbsetcache(cdb, opts->cachedrecords, 0, 0);
        }
        int bnum = 0;
        uint8_t tflags = 0;
        if (opts->records > 0) {
            bnum = tclmax(opts->records * 2 + 1, TDBDEFBNUM);
        }
        if (opts->large) {
            tflags |= TDBTLARGE;
        }
        if (opts->compressed) {
            tflags |= TDBTDEFLATE;
        }
        tctdbtune(cdb, bnum, 0, 0, tflags);
    }
    const char *mdbpath = jb->metadb->hdb->path;
    assert(mdbpath);
    TCXSTR *cxpath = tcxstrnew2(mdbpath);
    tcxstrcat2(cxpath, "_");
    tcxstrcat2(cxpath, colname);
    rv = tctdbopen(cdb, tcxstrptr(cxpath), jb->metadb->hdb->omode);
    *res = rv ? cdb : NULL;
    tcxstrdel(cxpath);
    return rv;
}

static void _ejdbclear(EJDB *jb) {
    assert(jb);
    jb->cdbs = NULL;
    jb->cdbsnum = 0;
    jb->metadb = NULL;
    jb->mmtx = NULL;
}

/* Check whether a string includes all tokens in another string.*/
static bool _qrycondcheckstrand(const char *vbuf, const TCLIST *tokens) {
    assert(vbuf && tokens);
    for (int i = 0; i < TCLISTNUM(tokens); ++i) {
        const char *token = TCLISTVALPTR(tokens, i);
        int tokensz = TCLISTVALSIZ(tokens, i);
        bool found = false;
        const char *str = vbuf;
        while (true) {
            const char *sp = str;
            while (*str != '\0' && !strchr(", ", *str)) {
                str++;
            }
            if (tokensz == (str - sp) && !strncmp(token, sp, tokensz)) { //Token matched
                found = true;
                break;
            }
            if (*str == '\0') break;
            str++;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

/* Check whether a string includes at least one tokens in another string.*/
static bool _qrycondcheckstror(const char *vbuf, const TCLIST *tokens) {
    for (int i = 0; i < TCLISTNUM(tokens); ++i) {
        const char *token = TCLISTVALPTR(tokens, i);
        int tokensz = TCLISTVALSIZ(tokens, i);
        bool found = false;
        const char *str = vbuf;
        while (true) {
            const char *sp = str;
            while (*str != '\0' && !strchr(", ", *str)) {
                str++;
            }
            if (tokensz == (str - sp) && !strncmp(token, sp, tokensz)) { //Token matched
                found = true;
                break;
            }
            if (*str == '\0') break;
            str++;
        }
        if (found) {
            return true;
        }
    }
    return false;
}

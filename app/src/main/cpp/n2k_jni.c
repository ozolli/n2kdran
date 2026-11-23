/*
 * n2k_jni.c — fine couche JNI entre Kotlin (oz.n2kdran.N2kCore) et libn2k.
 *
 * Le contexte est partagé entre le thread de réception (feedLine) et le
 * thread d'affichage (snapshot) : chaque appel prend le verrou.
 * Les données sont repérées par leur nom côté Kotlin (itemNames), pas par leur
 * numéro : l'ordre de l'enum C peut évoluer sans casser l'interface.
 */
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "n2k.h"

typedef struct {
    pthread_mutex_t lock;
    n2k_ctx        *ctx;
    n2k_polar      *polar;
} handle_t;

static handle_t *H(jlong h) { return (handle_t *)(intptr_t)h; }

/* Garde-fou : un contexte nul (déjà fermé) ne doit jamais être déréférencé. */
#define CHECK_H(h, ret) do { if (!(h)) return ret; } while (0)

JNIEXPORT jlong JNICALL
Java_oz_n2kdran_N2kCore_nativeCreate(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    handle_t *h = calloc(1, sizeof *h);
    if (!h)
        return 0;
    h->ctx = n2k_create();
    if (!h->ctx) {
        free(h);
        return 0;
    }
    pthread_mutex_init(&h->lock, NULL);
    return (jlong)(intptr_t)h;
}

JNIEXPORT void JNICALL
Java_oz_n2kdran_N2kCore_nativeDestroy(JNIEnv *env, jclass cls, jlong hh)
{
    (void)env; (void)cls;
    handle_t *h = H(hh);
    if (!h)
        return;
    n2k_destroy(h->ctx);
    n2k_polar_free(h->polar);
    pthread_mutex_destroy(&h->lock);
    free(h);
}

JNIEXPORT jboolean JNICALL
Java_oz_n2kdran_N2kCore_nativeFeedLine(JNIEnv *env, jclass cls, jlong hh,
                                       jstring line, jlong now_ms, jint flags)
{
    (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, JNI_FALSE);
    const char *s = (*env)->GetStringUTFChars(env, line, NULL);
    if (!s)
        return JNI_FALSE;
    pthread_mutex_lock(&h->lock);
    int ok = n2k_feed_line_ex(h->ctx, s, (uint64_t)now_ms, (unsigned)flags);
    pthread_mutex_unlock(&h->lock);
    (*env)->ReleaseStringUTFChars(env, line, s);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_oz_n2kdran_N2kCore_nativeSnapshot(JNIEnv *env, jclass cls, jlong hh, jlong now_ms,
                                       jdoubleArray values, jbyteArray status)
{
    (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, );
    double v[N2K_ITEM_COUNT];
    uint8_t st[N2K_ITEM_COUNT];
    pthread_mutex_lock(&h->lock);
    n2k_snapshot(h->ctx, (uint64_t)now_ms, v, st);
    pthread_mutex_unlock(&h->lock);
    jsize n = (*env)->GetArrayLength(env, values);
    if (n > N2K_ITEM_COUNT) n = N2K_ITEM_COUNT;
    (*env)->SetDoubleArrayRegion(env, values, 0, n, v);
    (*env)->SetByteArrayRegion(env, status, 0, n, (const jbyte *)st);
}

JNIEXPORT jobjectArray JNICALL
Java_oz_n2kdran_N2kCore_nativeItemNames(JNIEnv *env, jclass cls)
{
    (void)cls;
    jclass str = (*env)->FindClass(env, "java/lang/String");
    jobjectArray a = (*env)->NewObjectArray(env, N2K_ITEM_COUNT, str, NULL);
    for (int i = 0; i < N2K_ITEM_COUNT; i++) {
        jstring s = (*env)->NewStringUTF(env, n2k_item_name((n2k_item)i));
        (*env)->SetObjectArrayElement(env, a, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return a;
}

/* Installe une polaire (contenu texte du fichier). Retourne null si OK,
 * sinon le message d'erreur. L'ancienne polaire est libérée. */
JNIEXPORT jstring JNICALL
Java_oz_n2kdran_N2kCore_nativeSetPolar(JNIEnv *env, jclass cls, jlong hh, jbyteArray text)
{
    (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, NULL);
    char err[160] = "";
    jsize len = (*env)->GetArrayLength(env, text);
    jbyte *b = (*env)->GetByteArrayElements(env, text, NULL);
    if (!b)
        return (*env)->NewStringUTF(env, "mémoire insuffisante");
    n2k_polar *p = n2k_polar_parse((const char *)b, (size_t)len, err, sizeof err);
    (*env)->ReleaseByteArrayElements(env, text, b, JNI_ABORT);
    if (!p)
        return (*env)->NewStringUTF(env, err);
    pthread_mutex_lock(&h->lock);
    n2k_set_polar(h->ctx, p);
    n2k_polar *old = h->polar;
    h->polar = p;
    pthread_mutex_unlock(&h->lock);
    n2k_polar_free(old);
    return NULL;
}

JNIEXPORT void JNICALL
Java_oz_n2kdran_N2kCore_nativeSetStaleMs(JNIEnv *env, jclass cls, jlong hh, jint ms)
{
    (void)env; (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, );
    pthread_mutex_lock(&h->lock);
    n2k_set_stale_ms(h->ctx, (uint32_t)ms);
    pthread_mutex_unlock(&h->lock);
}

JNIEXPORT void JNICALL
Java_oz_n2kdran_N2kCore_nativeSetAisIgnoreStill(JNIEnv *env, jclass cls, jlong hh, jboolean on)
{
    (void)env; (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, );
    pthread_mutex_lock(&h->lock);
    n2k_set_ais_ignore_still(h->ctx, on ? 1 : 0);
    pthread_mutex_unlock(&h->lock);
}

/* Compteurs : lignes, rejetées, fp_ok, fp_abandonnés. */
JNIEXPORT void JNICALL
Java_oz_n2kdran_N2kCore_nativeStats(JNIEnv *env, jclass cls, jlong hh, jlongArray out)
{
    (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, );
    n2k_stats s;
    pthread_mutex_lock(&h->lock);
    n2k_get_stats(h->ctx, &s);
    pthread_mutex_unlock(&h->lock);
    jlong v[4] = { (jlong)s.lines, (jlong)s.bad_lines, (jlong)s.fp_complete, (jlong)s.fp_dropped };
    (*env)->SetLongArrayRegion(env, out, 0, 4, v);
}

/* Profil de VMG (181 valeurs, TWA 0..180°) ; false si indisponible. */
JNIEXPORT jboolean JNICALL
Java_oz_n2kdran_N2kCore_nativeVmgProfile(JNIEnv *env, jclass cls, jlong hh, jdoubleArray out)
{
    (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, JNI_FALSE);
    double pct[181];
    pthread_mutex_lock(&h->lock);
    int ok = n2k_vmg_profile(h->ctx, pct);
    pthread_mutex_unlock(&h->lock);
    if (ok)
        (*env)->SetDoubleArrayRegion(env, out, 0, 181, pct);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/* Nom du WP de destination s'il est frais, sinon null. */
JNIEXPORT jbyteArray JNICALL
Java_oz_n2kdran_N2kCore_nativeWpName(JNIEnv *env, jclass cls, jlong hh, jlong now_ms)
{
    (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, NULL);
    char name[64];
    pthread_mutex_lock(&h->lock);
    n2k_status st = n2k_wp_name(h->ctx, (uint64_t)now_ms, name, sizeof name);
    pthread_mutex_unlock(&h->lock);
    if (st != N2K_OK || !name[0])
        return NULL;
    /* Octets UTF-8 rendus tels quels, décodés côté Kotlin : NewStringUTF exige du
     * « Modified UTF-8 » et fait avorter le processus sur un octet inattendu. */
    jsize n = (jsize)strlen(name);
    jbyteArray out = (*env)->NewByteArray(env, n);
    if (out)
        (*env)->SetByteArrayRegion(env, out, 0, n, (const jbyte *)name);
    return out;
}

/* Cible AIS (out : niveau, MMSI, distance m, relèvement rad, CPA m, TCPA s,
 * position estimée 0/1) et
 * son nom en octets ASCII (tableau vide si inconnu) ; null si aucune. */
static jbyteArray ais_out(JNIEnv *env, int level, const n2k_ais_target *t, jdoubleArray out)
{
    if (!level)
        return NULL;
    jdouble v[7] = { level, (jdouble)t->mmsi, t->range, t->bearing, t->cpa, t->tcpa, t->est };
    (*env)->SetDoubleArrayRegion(env, out, 0, 7, v);
    jsize n = (jsize)strlen(t->name);
    jbyteArray name = (*env)->NewByteArray(env, n);
    if (name)
        (*env)->SetByteArrayRegion(env, name, 0, n, (const jbyte *)t->name);
    return name;
}

/* MMSI acquittés passés par Kotlin (au plus N2K_AIS_SKIP). */
#define N2K_AIS_SKIP 64
static int ais_skip(JNIEnv *env, jintArray skip, uint32_t *sk)
{
    if (!skip)
        return 0;
    jsize n = (*env)->GetArrayLength(env, skip);
    if (n > N2K_AIS_SKIP)
        n = N2K_AIS_SKIP;
    (*env)->GetIntArrayRegion(env, skip, 0, n, (jint *)sk);
    return (int)n;
}

/* Cible la plus menaçante (niveau 1 suivie, 2 dangereuse). */
JNIEXPORT jbyteArray JNICALL
Java_oz_n2kdran_N2kCore_nativeAisThreat(JNIEnv *env, jclass cls, jlong hh, jlong now_ms,
                                        jdouble cpa_max, jdouble tcpa_max, jdouble range_max,
                                        jintArray skip, jdoubleArray out)
{
    (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, NULL);
    n2k_ais_target t;
    uint32_t sk[N2K_AIS_SKIP];
    int nsk = ais_skip(env, skip, sk);
    pthread_mutex_lock(&h->lock);
    int lvl = n2k_ais_threat(h->ctx, (uint64_t)now_ms, cpa_max, tcpa_max, range_max, sk, nsk, &t);
    pthread_mutex_unlock(&h->lock);
    return ais_out(env, lvl, &t, out);
}

/* Oubli des cibles AIS et des hommes à la mer mémorisés (arrêt de l'application). */
JNIEXPORT void JNICALL
Java_oz_n2kdran_N2kCore_nativeAisReset(JNIEnv *env, jclass cls, jlong hh)
{
    (void)env; (void)cls;
    handle_t *h = H(hh);
    if (!h)
        return;
    pthread_mutex_lock(&h->lock);
    n2k_ais_reset(h->ctx);
    pthread_mutex_unlock(&h->lock);
}

/* Balise de détresse active la plus proche (niveau = 970 SART, 972 MOB, 974 EPIRB). */
JNIEXPORT jbyteArray JNICALL
Java_oz_n2kdran_N2kCore_nativeAisDistress(JNIEnv *env, jclass cls, jlong hh, jlong now_ms,
                                          jintArray skip, jdoubleArray out)
{
    (void)cls;
    handle_t *h = H(hh);
    CHECK_H(h, NULL);
    n2k_ais_target t;
    uint32_t sk[N2K_AIS_SKIP];
    int nsk = ais_skip(env, skip, sk);
    pthread_mutex_lock(&h->lock);
    int kind = n2k_ais_distress(h->ctx, (uint64_t)now_ms, sk, nsk, &t);
    pthread_mutex_unlock(&h->lock);
    return ais_out(env, kind, &t, out);
}

/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include <string.h>

#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/opensslv.h>

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "crypto.h"
#include "../or/or.h"
#include "log.h"

#if OPENSSL_VERSION_NUMBER < 0x00905000l
#error "We require openssl >= 0.9.5"
#elif OPENSSL_VERSION_NUMBER < 0x00906000l
#define OPENSSL_095
#endif

/*
 * Certain functions that return a success code in OpenSSL 0.9.6 return void
 * (and don't indicate errors) in OpenSSL version 0.9.5.
 *
 * [OpenSSL 0.9.5 matters, because it ships with Redhat 6.2.]
 */
#ifdef OPENSSL_095
#define RETURN_SSL_OUTCOME(exp) (exp); return 0
#else
#define RETURN_SSL_OUTCOME(exp) return !(exp)
#endif

static inline int 
crypto_cipher_iv_length(int type) {
  switch(type) 
    {
    case CRYPTO_CIPHER_IDENTITY: return 0;
    case CRYPTO_CIPHER_DES: return 8;
    case CRYPTO_CIPHER_RC4: return 16;
    case CRYPTO_CIPHER_3DES: return 8;
    default: assert(0); return -1;
    }
}

static inline int
crypto_cipher_key_length(int type) {
  switch(type) 
    {
    case CRYPTO_CIPHER_IDENTITY: return 0;
    case CRYPTO_CIPHER_DES: return 8;
    case CRYPTO_CIPHER_RC4: return 16;
    case CRYPTO_CIPHER_3DES: return 16;
    default: assert(0); return -1;
    }
}

static inline EVP_CIPHER *
crypto_cipher_evp_cipher(int type, int enc) {
  switch(type) 
    {
    case CRYPTO_CIPHER_IDENTITY: return EVP_enc_null();
    case CRYPTO_CIPHER_DES: return EVP_des_ofb();
    case CRYPTO_CIPHER_RC4: return EVP_rc4();
    case CRYPTO_CIPHER_3DES: return EVP_des_ede_ofb();
    default: return NULL;
    }
}

int crypto_global_init() 
{
  ERR_load_crypto_strings();
  return 0;
}

int crypto_global_cleanup()
{
  ERR_free_strings();
  return 0;
}

crypto_pk_env_t *crypto_new_pk_env(int type)
{
  crypto_pk_env_t *env;
  
  env = (crypto_pk_env_t *)malloc(sizeof(crypto_pk_env_t));
  if (!env)
    return 0;
  
  env->type = type;
  env->refs = 1;
  env->key = NULL;
  env->aux = NULL;
  
  switch(type) {
    case CRYPTO_PK_RSA:
    env->key = (unsigned char *)RSA_new();
    if (!env->key) {
      free((void *)env);
      return NULL;
    }
    break;
    default:
    free((void *)env);
    return NULL;
    break;
  }

  return env;
}

void crypto_free_pk_env(crypto_pk_env_t *env)
{
  assert(env);

  if(--env->refs > 0)
    return;
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
    if (env->key)
      RSA_free((RSA *)env->key);
    break;
    default:
    break;
  }
  
  free((void *)env);
  return;
}


/* Create a new crypto_cipher_env_t for a given onion cipher type, key,
 * iv, and encryption flag (1=encrypt, 0=decrypt).  Return the crypto object
 * on success; NULL on failure.
 */
crypto_cipher_env_t *
crypto_create_init_cipher(int cipher_type, char *key, char *iv, int encrypt_mode) 
{
  int r;
  crypto_cipher_env_t *crypto = NULL;

  if (! (crypto = crypto_new_cipher_env(cipher_type))) {
    log(LOG_ERR, "Unable to allocate crypto object");
    return NULL;
  }

  if (crypto_cipher_set_key(crypto, key)) {
    log(LOG_ERR, "Unable to set key: %s", crypto_perror());
    goto error;
  }

  if (crypto_cipher_set_iv(crypto, iv)) {
    log(LOG_ERR, "Unable to set iv: %s", crypto_perror());
    goto error;
  }
  
  if (encrypt_mode)
    r = crypto_cipher_encrypt_init_cipher(crypto);
  else
    r = crypto_cipher_decrypt_init_cipher(crypto);

  if (r) {
    log(LOG_ERR, "Unabble to initialize cipher: %s", crypto_perror());
    goto error;
  }
  return crypto;

 error:
  if (crypto)
    crypto_free_cipher_env(crypto);
  return NULL;
}

crypto_cipher_env_t *crypto_new_cipher_env(int type)
{
  crypto_cipher_env_t *env;
  int iv_len, key_len;
  
  env = (crypto_cipher_env_t *)malloc(sizeof(crypto_cipher_env_t));
  if (!env)
    return NULL;
  
  env->type = type;
  env->key = NULL;
  env->iv = NULL;
  env->aux = NULL;

  iv_len = crypto_cipher_iv_length(type);
  key_len = crypto_cipher_key_length(type);
  
  if (! crypto_cipher_evp_cipher(type,0))
    /* This is not an openssl cipher */
    goto err;
  else {
    env->aux = (unsigned char *)malloc(sizeof(EVP_CIPHER_CTX));
    EVP_CIPHER_CTX_init((EVP_CIPHER_CTX *)env->aux);
  }

  if (iv_len && !(env->iv = (unsigned char *)malloc(iv_len)))
    goto err;

  if (key_len && !(env->key = (unsigned char *)malloc(key_len)))
    goto err;
  
  return env;
err:
  if (env->key)
    free(env->key);
  if (env->iv)
    free(env->iv);
  if (env->aux)
    free(env->aux);
  if (env)
    free(env);
  return NULL;
}

void crypto_free_cipher_env(crypto_cipher_env_t *env)
{
  assert(env);

  if (crypto_cipher_evp_cipher(env->type,0)) {
    /* This is an openssl cipher */
    assert(env->aux);
    EVP_CIPHER_CTX_cleanup((EVP_CIPHER_CTX *)env->aux);
  }

  if (env->aux)
    free((void *)env->aux);
  if (env->iv) 
    free((void *)env->iv);
  if (env->key)
    free((void *)env->key);

  free((void *)env);
}

/* public key crypto */
int crypto_pk_generate_key(crypto_pk_env_t *env)
{
  assert(env);
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
    if (env->key)
      RSA_free((RSA *)env->key);
    env->key = (unsigned char *)RSA_generate_key(1024,65537, NULL, NULL);
    if (!env->key)
      return -1;
    break;
    default:
    return -1;
  }
  
  return 0;
}

int crypto_pk_read_private_key_from_file(crypto_pk_env_t *env, FILE *src)
{
  assert(env && src);
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
    if (env->key)
      RSA_free((RSA *)env->key);
    env->key = (unsigned char *)PEM_read_RSAPrivateKey(src, NULL, NULL, NULL);
    if (!env->key)
      return -1;
    break;
    default :
    return -1;
  }
  
  return 0;
}

int crypto_pk_read_private_key_from_filename(crypto_pk_env_t *env, unsigned char *keyfile)
{
  FILE *f_pr;
  int retval = 0;
 
  assert(env && keyfile);
  
  if (strspn(keyfile,CONFIG_LEGAL_FILENAME_CHARACTERS) == strlen(keyfile)) /* filename contains legal characters only */
  {
    /* open the keyfile */
    f_pr=fopen(keyfile,"rb");
    if (!f_pr)
      return -1;
    
    /* read the private key */
    retval = crypto_pk_read_private_key_from_file(env, f_pr);
    fclose(f_pr);
    if (retval == -1)
    {
      log(LOG_ERR,"Error reading private key : %s",crypto_perror());
      return -1;
    }
      
    /* check the private key */
    retval = crypto_pk_check_key(env);
    if (retval == 0)
    {
      log(LOG_ERR,"Private key read but is invalid : %s.", crypto_perror());
      return -1;
    }
    else if (retval == -1)
    {
      log(LOG_ERR,"Private key read but validity checking failed : %s",crypto_perror());
      return -1;
    }
    else if (retval == 1)
    {
      return 0;
    }
  } /* filename contains legal characters only */
  
  return -1; /* report error */
}

int crypto_pk_read_public_key_from_file(crypto_pk_env_t *env, FILE *src)
{
  assert(env && src);
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
    if(env->key)
      RSA_free((RSA *)env->key); 
    env->key = (unsigned char *)PEM_read_RSAPublicKey(src, NULL, NULL, NULL);
    if (!env->key)
      return -1;
    break;
    default :
    return -1;
  }
  
  return 0;
}

int crypto_pk_write_public_key_to_string(crypto_pk_env_t *env, char **dest, int *len) {
  BUF_MEM *buf;
  BIO *b; 

  assert(env && env->key && dest);

  switch(env->type) {
    case CRYPTO_PK_RSA:

        b = BIO_new(BIO_s_mem()); /* Create a memory BIO */

        /* Now you can treat b as if it were a file.  Just use the
         *          * PEM_*_bio_* functions instead of the non-bio variants.
         *                   */
        if(!PEM_write_bio_RSAPublicKey(b, (RSA *)env->key))
          return -1;

        BIO_get_mem_ptr(b, &buf);
        BIO_set_close(b, BIO_NOCLOSE); /* so BIO_free doesn't free buf */
        BIO_free(b);

        *dest = malloc(buf->length+1);
        if(!*dest)
          return -1;
        memcpy(*dest, buf->data, buf->length);
        (*dest)[buf->length] = 0; /* null terminate it */
        *len = buf->length;
        BUF_MEM_free(buf);

      break;
    default:
      return -1;
  }
  
  return 0;
}

int crypto_pk_read_public_key_from_string(crypto_pk_env_t *env, char *src, int len) {
  BIO *b; 

  assert(env && src);

  switch(env->type) {
    case CRYPTO_PK_RSA:
      b = BIO_new(BIO_s_mem()); /* Create a memory BIO */

      BIO_write(b, src, len);

      RSA_free((RSA *)env->key); 
      env->key = (unsigned char *)PEM_read_bio_RSAPublicKey(b, NULL, NULL, NULL);
      if(!env->key)
        return -1;

      BIO_free(b);
      break;
    default:
      return -1;
  }
  
  return 0;
}

int crypto_pk_write_private_key_to_file(crypto_pk_env_t *env, FILE *dest)
{
  assert(env && dest);
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
    if (!env->key)
      return -1;
    if (PEM_write_RSAPrivateKey(dest, (RSA *)env->key, NULL, NULL, 0,0, NULL) == 0)
      return -1;
    break;
    default :
    return -1;
  }
  
  return 0;
}
int crypto_pk_write_public_key_to_file(crypto_pk_env_t *env, FILE *dest)
{
  assert(env && dest);
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
    if (!env->key)
      return -1;
    if (PEM_write_RSAPublicKey(dest, (RSA *)env->key) == 0)
      return -1;
    break;
    default :
    return -1;
  }
  
  return 0;
}

int crypto_pk_check_key(crypto_pk_env_t *env)
{
  assert(env);
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
      return RSA_check_key((RSA *)env->key);
    default:
      return -1;
  }
}

int crypto_pk_set_key(crypto_pk_env_t *env, unsigned char *key)
{
  assert(env && key);
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
    if (!env->key)
      return -1;
    /* XXX BUG XXX you can't memcpy an RSA, it's got a bunch of subpointers */
    assert(0);
    memcpy((void *)env->key, (void *)key, sizeof(RSA));
    break;
    default :
    return -1;
  }
  
  return 0;
}

int crypto_pk_cmp_keys(crypto_pk_env_t *a, crypto_pk_env_t *b) {
  int result; 
  
  if (!a || !b)
    return -1;
  
  if (!a->key || !b->key)
    return -1;
  
  if (a->type != b->type)
    return -1;
  
  switch(a->type) {
    case CRYPTO_PK_RSA:
    assert(((RSA *)a->key)->n && ((RSA *)a->key)->e && ((RSA *)b->key)->n && ((RSA *)b->key)->e);
    result = BN_cmp(((RSA *)a->key)->n, ((RSA *)b->key)->n);
    if (result)
      return result;
    return BN_cmp(((RSA *)a->key)->e, ((RSA *)b->key)->e);
    default:
    return -1;
  }
}

int crypto_pk_keysize(crypto_pk_env_t *env)
{
  assert(env && env->key);
  
  return RSA_size((RSA *)env->key);
}

crypto_pk_env_t *crypto_pk_dup_key(crypto_pk_env_t *env) {
  assert(env && env->key);

  switch(env->type) {
    case CRYPTO_PK_RSA:
      env->refs++; 
      break;
    default:
      return NULL; 
  }
  
  return env;
}

int crypto_pk_public_encrypt(crypto_pk_env_t *env, unsigned char *from, int fromlen, unsigned char *to, int padding)
{
  assert(env && from && to);
  
  switch(env->type) {
    case CRYPTO_PK_RSA:
    return RSA_public_encrypt(fromlen, from, to, (RSA *)env->key, padding);
    default:
    return -1;
  }
}

int crypto_pk_private_decrypt(crypto_pk_env_t *env, unsigned char *from, int fromlen, unsigned char *to, int padding)
{
  assert(env && from && to);

  switch(env->type) {
  case CRYPTO_PK_RSA:
    if (!(((RSA*)env->key)->p))
      return -1;
    return RSA_private_decrypt(fromlen, from, to, (RSA *)env->key, padding);
    default:
    return -1;
  }
}

/* symmetric crypto */
int crypto_cipher_generate_key(crypto_cipher_env_t *env)
{
  int key_len;
  assert(env);

  key_len = crypto_cipher_key_length(env->type);

  if (key_len > 0)
    return crypto_rand(key_len, env->key);
  else if (key_len == 0)
    return 0;
  else
    return -1;
}

int crypto_cipher_set_iv(crypto_cipher_env_t *env, unsigned char *iv)
{
  int iv_len;
  assert(env && iv);
  
  iv_len = crypto_cipher_iv_length(env->type);
  if (!iv_len)
    return 0;

  if (!env->iv)
    return -1;

  memcpy((void*)env->iv, (void*)iv, iv_len);
  
  return 0;
}

int crypto_cipher_set_key(crypto_cipher_env_t *env, unsigned char *key)
{
  int key_len;
  assert(env && key);

  key_len = crypto_cipher_key_length(env->type);
  if (!key_len)
    return 0;

  if (!env->key)
    return -1;

  memcpy((void*)env->key, (void*)key, key_len);
  
  return 0;
}

int crypto_cipher_encrypt_init_cipher(crypto_cipher_env_t *env)
{
  assert(env);
  
  if (crypto_cipher_evp_cipher(env->type, 1)) {
    RETURN_SSL_OUTCOME(EVP_EncryptInit((EVP_CIPHER_CTX *)env->aux,
				       crypto_cipher_evp_cipher(env->type, 1),
				       env->key, env->iv));
  } else {
    return -1;
  }
}

int crypto_cipher_decrypt_init_cipher(crypto_cipher_env_t *env)
{
  assert(env);

  if (crypto_cipher_evp_cipher(env->type, 0)) {
    RETURN_SSL_OUTCOME(EVP_EncryptInit((EVP_CIPHER_CTX *)env->aux,
				       crypto_cipher_evp_cipher(env->type, 0),
				       env->key, env->iv));
  } else {
    return -1;
  }
}

int crypto_cipher_encrypt(crypto_cipher_env_t *env, unsigned char *from, unsigned int fromlen, unsigned char *to)
{
  int tolen;
  
  assert(env && from && to);
  
  RETURN_SSL_OUTCOME(EVP_EncryptUpdate((EVP_CIPHER_CTX *)env->aux, to, &tolen, from, fromlen));
}

int crypto_cipher_decrypt(crypto_cipher_env_t *env, unsigned char *from, unsigned int fromlen, unsigned char *to)
{
  int tolen;
  
  assert(env && from && to);
  
  RETURN_SSL_OUTCOME(EVP_DecryptUpdate((EVP_CIPHER_CTX *)env->aux, to, &tolen, from, fromlen));
}

/* SHA-1 */
int crypto_SHA_digest(unsigned char *m, int len, unsigned char *digest)
{
  assert(m && digest);
  return (SHA1(m,len,digest) == NULL);
}

/* random numbers */
int crypto_rand(unsigned int n, unsigned char *to)
{
  assert(to);
  return (RAND_bytes(to, n) != 1);
}

int crypto_pseudo_rand(unsigned int n, unsigned char *to)
{
  assert(to);
  return (RAND_pseudo_bytes(to, n) == -1);
}

/* errors */
char *crypto_perror()
{
  return (char *)ERR_reason_error_string(ERR_get_error());
}

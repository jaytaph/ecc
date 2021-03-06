#include <string.h>
#include <node.h>
#include <node_buffer.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/ecdh.h>

#include "eckey.h"

using namespace v8;
using namespace node;


static Handle<Value> V8Exception(const char* msg) {
	HandleScope scope;
	return ThrowException(Exception::Error(String::New(msg)));
}

// Not sure where this came from. but looks like a function that should be part of openssl
int static inline EC_KEY_regenerate_key(EC_KEY *eckey, const BIGNUM *priv_key) {
	if (!eckey) return 0;
	int ok = 0;
	BN_CTX *ctx = NULL;
	EC_POINT *pub_key = NULL;
	const EC_GROUP *group = EC_KEY_get0_group(eckey);
	if ((ctx = BN_CTX_new()) == NULL)
		goto err;
	pub_key = EC_POINT_new(group);
	if (pub_key == NULL)
		goto err;
	if (!EC_POINT_mul(group, pub_key, priv_key, NULL, NULL, ctx))
		goto err;
	EC_KEY_set_private_key(eckey, priv_key);
	EC_KEY_set_public_key(eckey, pub_key);
	ok = 1;
err:
	if (pub_key)
		EC_POINT_free(pub_key);
	if (ctx != NULL)
		BN_CTX_free(ctx);
	return ok;
}

ECKey::ECKey(int curve) {
	mLastError = NULL;
	mHasPrivateKey = false;
	mCurve = curve;
	mKey = EC_KEY_new_by_curve_name(mCurve);
	if (!mKey) {
		V8Exception("EC_KEY_new_by_curve_name Invalid curve?");
		return;
	}
}
ECKey::~ECKey() {
	if (mKey) {
		EC_KEY_free(mKey);
	}
}

// Node module init
void ECKey::Init(Handle<Object> exports) {
	Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
	tpl->SetClassName(String::NewSymbol("ECKey"));
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	//Accessors
	tpl->InstanceTemplate()->SetAccessor(String::NewSymbol("LastError"), GetLastError);
	tpl->InstanceTemplate()->SetAccessor(String::NewSymbol("HasPrivateKey"), GetHasPrivateKey);
	tpl->InstanceTemplate()->SetAccessor(String::NewSymbol("PublicKey"), GetPublicKey);
	tpl->InstanceTemplate()->SetAccessor(String::NewSymbol("PrivateKey"), GetPrivateKey);

	//Methods (Prototype)
	tpl->PrototypeTemplate()->Set(String::NewSymbol("sign"), FunctionTemplate::New(Sign)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("verifySignature"), FunctionTemplate::New(VerifySignature)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("deriveSharedSecret"), FunctionTemplate::New(DeriveSharedSecret)->GetFunction());

	Persistent<Function> constructor = Persistent<Function>::New(tpl->GetFunction());
	exports->Set(String::NewSymbol("ECKey"), constructor);
}

// Node constructor function
Handle<Value> ECKey::New(const Arguments &args) {
	if (!args.IsConstructCall()) {
		return V8Exception("Must use new keyword");
	}
	if (args[0]->IsUndefined()) {
		return V8Exception("Curve required?");
	}
	HandleScope scope;
	ECKey *eckey = new ECKey(args[0]->NumberValue());
	if (!args[1]->IsUndefined()) {
		//we have a second parameter, check the third to see if it is public or private.
		if (!Buffer::HasInstance(args[1])) {
			return V8Exception("Second parameter must be a buffer");
		}
		Handle<Object> buffer = args[1]->ToObject();
		const unsigned char *bufferData = (unsigned char *) Buffer::Data(buffer);
		if ((args[2]->IsUndefined()) || (args[2]->BooleanValue() == false)) {
			// it's a private key
			BIGNUM *bn = BN_bin2bn(bufferData, Buffer::Length(buffer), BN_new());
			if (EC_KEY_regenerate_key(eckey->mKey, bn) == 0) {
				BN_clear_free(bn);
				return V8Exception("Invalid private key");
			}
			BN_clear_free(bn);
			eckey->mHasPrivateKey = true;
		} else {
			// it's a public key
			if (!o2i_ECPublicKey(&(eckey->mKey), &bufferData, Buffer::Length(buffer))) {
				return V8Exception("o2i_ECPublicKey failed");
			}
		}
	} else {
		if (!EC_KEY_generate_key(eckey->mKey)) {
			return V8Exception("EC_KEY_generate_key failed");
		}
		eckey->mHasPrivateKey = true;
	}
	eckey->Wrap(args.This());
	return scope.Close(args.This());
}

// Node properity functions
Handle<Value> ECKey::GetLastError(Local<String> property, const AccessorInfo &info) {
	HandleScope scope;
	ECKey *eckey = ObjectWrap::Unwrap<ECKey>(info.Holder());
	return scope.Close(String::New(eckey->mLastError));
}
Handle<Value> ECKey::GetHasPrivateKey(Local<String> property, const AccessorInfo &info) {
	HandleScope scope;
	ECKey *eckey = ObjectWrap::Unwrap<ECKey>(info.Holder());
	return scope.Close(Boolean::New(eckey->mHasPrivateKey));
}
Handle<Value> ECKey::GetPublicKey(Local<String> property, const AccessorInfo &info) {
	ECKey *eckey = ObjectWrap::Unwrap<ECKey>(info.Holder());
	const EC_GROUP *group = EC_KEY_get0_group(eckey->mKey);
	const EC_POINT *point = EC_KEY_get0_public_key(eckey->mKey);
	unsigned int nReq = EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, NULL, 0, NULL);
	if (!nReq) {
		return V8Exception("EC_POINT_point2oct error");
	}
	unsigned char *buf, *buf2;
	buf = buf2 = (unsigned char *)malloc(nReq);
	if (EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, buf, nReq, NULL) != nReq) {
		return V8Exception("EC_POINT_point2oct didn't return correct size");
	}
	HandleScope scope;
	Buffer *buffer = Buffer::New(nReq);
	memcpy(Buffer::Data(buffer), buf2, nReq);
	free(buf2);
	return scope.Close(buffer->handle_);
}
Handle<Value> ECKey::GetPrivateKey(Local<String> property, const AccessorInfo &info) {
	ECKey *eckey = ObjectWrap::Unwrap<ECKey>(info.Holder());
	const BIGNUM *bn = EC_KEY_get0_private_key(eckey->mKey);
	if (bn == NULL) {
		return V8Exception("EC_KEY_get0_private_key failed");
	}
	int priv_size = BN_num_bytes(bn);
	unsigned char *priv_buf = (unsigned char *)malloc(priv_size);
	int n = BN_bn2bin(bn, priv_buf);
	if (n != priv_size) {
		return V8Exception("BN_bn2bin didn't return priv_size");
	}
	HandleScope scope;
	Buffer *buffer = Buffer::New(priv_size);
	memcpy(Buffer::Data(buffer), priv_buf, priv_size);
	free(priv_buf);
	return scope.Close(buffer->handle_);
}

// Node method functions
Handle<Value> ECKey::Sign(const Arguments &args) {
	HandleScope scope;
	ECKey * eckey = ObjectWrap::Unwrap<ECKey>(args.This());
	if (!Buffer::HasInstance(args[0])) {
		return V8Exception("digest must be a buffer");
	}
	if (!eckey->mHasPrivateKey) {
		return V8Exception("cannot sign without private key");
	}
	Handle<Object> digest = args[0]->ToObject();
	const unsigned char *digest_data = (unsigned char *)Buffer::Data(digest);
	unsigned int digest_len = Buffer::Length(digest);

	ECDSA_SIG *sig = ECDSA_do_sign(digest_data, digest_len, eckey->mKey);
	if (!sig) {
		return V8Exception("ECDSA_do_sign");
	}
	int sig_len = i2d_ECDSA_SIG(sig, NULL);
	if (!sig_len) {
		return V8Exception("i2d_ECDSA_SIG");
	}
	unsigned char *sig_data, *sig_data2;
	sig_data = sig_data2 = (unsigned char *)malloc(sig_len);
	if (i2d_ECDSA_SIG(sig, &sig_data) != sig_len) {
		ECDSA_SIG_free(sig);
		free(sig_data2);
		return V8Exception("i2d_ECDSA_SIG didnot return correct length");
	}
	ECDSA_SIG_free(sig);
	Buffer *result = Buffer::New(sig_len);
	memcpy(Buffer::Data(result), sig_data2, sig_len);
	free(sig_data2);
	return scope.Close(result->handle_);
}
Handle<Value> ECKey::VerifySignature(const Arguments &args) {
	HandleScope scope;
	ECKey *eckey = ObjectWrap::Unwrap<ECKey>(args.This());
	if (!Buffer::HasInstance(args[0])) {
		return V8Exception("digest must be a buffer");
	}
	if (!Buffer::HasInstance(args[1])) {
		return V8Exception("signature must be a buffer");
	}
	Handle<Object> digest = args[0]->ToObject();
	Handle<Object> signature = args[1]->ToObject();
	const unsigned char *digest_data = (unsigned char *)Buffer::Data(digest);
	const unsigned char *signature_data = (unsigned char *)Buffer::Data(signature);
	unsigned int digest_len = Buffer::Length(digest);
	unsigned int signature_len = Buffer::Length(signature);
	int result = ECDSA_verify(0, digest_data, digest_len, signature_data, signature_len, eckey->mKey);
	if (result == -1) {
		return V8Exception("ECDSA_verify");
	} else if (result == 0) {
		return scope.Close(Boolean::New(false));
	} else if (result == 1) {
		return scope.Close(Boolean::New(true));
	} else {
		return V8Exception("ECDSA_verify gave an unexpected return value");
	}
}
Handle<Value> ECKey::DeriveSharedSecret(const Arguments &args) {
	HandleScope scope;
	if (args[0]->IsUndefined()) {
		return V8Exception("other is required");
	}
	ECKey *eckey = ObjectWrap::Unwrap<ECKey>(args.This());
	ECKey *other = ObjectWrap::Unwrap<ECKey>(args[0]->ToObject());
	if (!other) {
		return V8Exception("other must be an ECKey");
	}
	unsigned char *secret = (unsigned char*)malloc(512);
	int len = ECDH_compute_key(secret, 512, EC_KEY_get0_public_key(other->mKey), eckey->mKey, NULL);
	Buffer *result = Buffer::New(len);
	memcpy(Buffer::Data(result), secret, len);
	free(secret);
	return scope.Close(result->handle_);
}


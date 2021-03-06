/**
 * Copyright 2013 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include<cstdio>
#include<cstring>
#include<fstream>
#include<functional>
#include<iomanip>
#include<iostream>
#include<map>
#include<sstream>
#include<stdexcept>
#include<string>
#include<vector>

#include"tss/tspi.h"
#include"trousers/trousers.h"

#include"common.h"
#include"tspiwrap.h"
#include"internal.h"

std::ostream& operator<<(std::ostream& o, struct stpm::Key& key)
{
  o << "mod=" << stpm::to_hex(key.modulus)
    << ",exp=" << stpm::to_hex(key.exponent)
    << ",blob=" << stpm::to_hex(key.blob);
  return o;
}

BEGIN_NAMESPACE(stpm);

TSS_UUID srk_uuid = TSS_UUID_SRK;

// Wrap Tspi_* calls, checking return value and throwing exception.
// TODO: Adding debug logging.
TSS_RESULT
tscall(const std::string& name, std::function<TSS_RESULT()> func)
{
  TSS_RESULT res;
  if (TSS_SUCCESS != (res = func())) {
    throw std::runtime_error(name + "(): " + parseError(res));
  }
  return res;
}

std::string
xctime()
{
  time_t t;
  time(&t);
  char buf[128] = {0};
  ctime_r(&t, buf);
  while (strlen(buf) && buf[strlen(buf)-1] == '\n') {
    buf[strlen(buf)-1] = 0;
  }
  return buf;
}

bool
log_stderr()
{
  const char *doit{getenv("SIMPLE_TPM_PK11_LOG_STDERR")};
  return !!doit;
}

void
do_log(std::ostream* o, const std::string& msg)
{
  *o << msg << std::endl;
  if (log_stderr()) {
    std::cerr << msg << std::endl;
  }
}

int
keysize_flag(int bits) {
  switch (bits) {
  case 512:
    return TSS_KEY_SIZE_512;
  case 1024:
    return TSS_KEY_SIZE_1024;
  case 2048:
    return TSS_KEY_SIZE_2048;
  case 4096:
    return TSS_KEY_SIZE_4096;
  case 8192:
    return TSS_KEY_SIZE_8192;
  case 16384:
    return TSS_KEY_SIZE_16384;
  }
  throw std::runtime_error("Unknown key size: " + std::to_string(bits) + "bit");
}

std::string
parseError(int code)
{
  const std::string layer{Trspi_Error_Layer(code)};
  const std::string err{Trspi_Error_String(code)};

  std::stringstream ss;
  ss << "Code=0x"
     << std::setw(8) << std::setbase(16) << std::setfill('0') << code
     << ": " << layer
     << ": " << err;
  return ss.str();
}

// TODO: Complete this implementatino.
Key
wrap_key(const std::string* srk_pin, const std::string* key_pin,
         const SoftwareKey& swkey)
{
  TPMStuff stuff{srk_pin};

  // === Set up key object ===
  int init_flags =
    TSS_KEY_TYPE_SIGNING
    | TSS_KEY_SIZE_2048
    | TSS_KEY_VOLATILE
    | TSS_KEY_NO_AUTHORIZATION
    | TSS_KEY_NOT_MIGRATABLE;

  TSS_HKEY key;
  TSCALL(Tspi_Context_CreateObject, stuff.ctx(),
         TSS_OBJECT_TYPE_RSAKEY, init_flags, &key);
  TSS_HPOLICY key_policy;
  TSCALL(Tspi_Context_CreateObject, stuff.ctx(),
         TSS_OBJECT_TYPE_POLICY, TSS_POLICY_USAGE, &key_policy);

  // Set modulus
  // TODO: Set modulus.

  // set private
  TSCALL(Tspi_SetAttribData, key, TSS_TSPATTRIB_KEY_BLOB,
         TSS_TSPATTRIB_KEYBLOB_PRIVATE_KEY,
         swkey.key.size(), (BYTE*)swkey.key.data());

  // Create key
  TSCALL(Tspi_Key_WrapKey, key, stuff.srk(), 0);

  Key ret;
  ret.modulus = swkey.modulus;
  ret.exponent = swkey.exponent;

  // Get keyblob.
  UINT32 blob_size;
  BYTE* blob_blob;
  TSCALL(Tspi_GetAttribData, key,
         TSS_TSPATTRIB_KEY_BLOB, TSS_TSPATTRIB_KEYBLOB_BLOB,
         &blob_size, &blob_blob);
  ret.blob = std::string{std::string(blob_blob, blob_blob+blob_size)};
  return ret;
}

Key
generate_key(const std::string* srk_pin, const std::string* key_pin, int bits) {
  TPMStuff stuff{srk_pin};

  {
    std::vector<char> buf(32);  // 256 bits.
    std::ifstream f;
    f.rdbuf()->pubsetbuf(nullptr, 0);
    f.open("/dev/urandom", std::ios::binary);
    if (!f.good()) {
      throw std::runtime_error("Failed to open /dev/random");
    }
    f.read(&buf[0], buf.size());
    if (f.fail() || f.eof()) {
      throw std::runtime_error("EOF in /dev/random");
    }
    if (static_cast<size_t>(f.gcount()) != buf.size()) {
      throw std::runtime_error("Short full read from /dev/random");
    }
    TSCALL(Tspi_TPM_StirRandom, stuff.tpm(),
           buf.size(), (BYTE*)&buf[0]);
  }

  // === Set up key object ===
  int init_flags =
    TSS_KEY_TYPE_SIGNING
    | keysize_flag(bits)
    | TSS_KEY_VOLATILE
    | TSS_KEY_NOT_MIGRATABLE;

  if (key_pin) {
    init_flags |= TSS_KEY_AUTHORIZATION;
  } else {
    init_flags |= TSS_KEY_NO_AUTHORIZATION;
  }

  TSS_HKEY key;
  TSCALL(Tspi_Context_CreateObject, stuff.ctx(),
         TSS_OBJECT_TYPE_RSAKEY, init_flags, &key);
  TSS_HPOLICY key_policy;
  TSCALL(Tspi_Context_CreateObject, stuff.ctx(),
         TSS_OBJECT_TYPE_POLICY, TSS_POLICY_USAGE, &key_policy);

  if (key_pin) {
    TSCALL(Tspi_Policy_SetSecret, key_policy,
           TSS_SECRET_MODE_PLAIN,
           key_pin->size(),
           (BYTE*)key_pin->data());
  } else {
    BYTE wks[] = TSS_WELL_KNOWN_SECRET;
    int wks_size = sizeof(wks);
    TSCALL(Tspi_Policy_SetSecret, key_policy,
           TSS_SECRET_MODE_SHA1, wks_size, wks);
  }
  TSCALL(Tspi_Policy_AssignToObject, key_policy, key);

  // Need to set DER mode for signing.
  TSCALL(Tspi_SetAttribUint32,key,
         TSS_TSPATTRIB_KEY_INFO,
         TSS_TSPATTRIB_KEYINFO_SIGSCHEME,
         TSS_SS_RSASSAPKCS1V15_DER);

  // === Create Key ===
  TSCALL(Tspi_Key_CreateKey, key, stuff.srk(), 0);

  Key ret;
  // Get modulus.
  UINT32 mod_size;
  BYTE* mod_blob;
  TSCALL(Tspi_GetAttribData, key,
         TSS_TSPATTRIB_RSAKEY_INFO, TSS_TSPATTRIB_KEYINFO_RSA_MODULUS,
         &mod_size, &mod_blob);
  std::clog << "Modulus size: " << mod_size << std::endl;
  ret.modulus = std::string(std::string(mod_blob, mod_blob+mod_size));

  // Get exponent.
  UINT32 exp_size;
  BYTE* exp_blob;
  TSCALL(Tspi_GetAttribData, key,
         TSS_TSPATTRIB_RSAKEY_INFO, TSS_TSPATTRIB_KEYINFO_RSA_EXPONENT,
         &exp_size, &exp_blob);
  std::clog << "Exponent size: " << exp_size << std::endl;
  ret.exponent = std::string{std::string(exp_blob, exp_blob+exp_size)};

  // Get keysize.
  UINT32 size;
  TSCALL(Tspi_GetAttribUint32, key,
         TSS_TSPATTRIB_RSAKEY_INFO, TSS_TSPATTRIB_KEYINFO_RSA_KEYSIZE,
         &size);
  std::clog << "Size: " << size << std::endl;

  // Get keyblob.
  UINT32 blob_size;
  BYTE* blob_blob;
  TSCALL(Tspi_GetAttribData, key,
         TSS_TSPATTRIB_KEY_BLOB, TSS_TSPATTRIB_KEYBLOB_BLOB,
         &blob_size, &blob_blob);
  std::clog << "Blob size: " << blob_size << std::endl;
  ret.blob = std::string{std::string(blob_blob, blob_blob+blob_size)};
  return ret;
}

std::string
to_hex(const std::string& s)
{
  std::stringstream ss;
  for (auto c : s) {
    ss << std::setw(2) << std::setfill('0') << std::setbase(16)
       << unsigned(c & 0xff);
  }
  return ss.str();
}

std::string
to_bin(const std::string& s)
{
  std::map<std::string, unsigned char> m;
  for (int c = 0; c < 256; c++) {
    unsigned char t[2] = {(unsigned char)c, 0};
    m[to_hex((char*)t)] = c;
  }

  if (s.size() & 1) {
    throw std::runtime_error("to_bin() on odd length string");
  }
  std::string ret;
  for (unsigned c = 0; c < s.size(); c+=2) {
    auto t = s.substr(c, 2);
    ret += m[t];
  }
  return ret;
}

Key
parse_keyfile(const std::string &s)
{
  std::istringstream ss(s);
  Key key;
  while (!ss.eof()) {
    std::string line;
    getline(ss, line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream linetokens{line};
    std::string cmd, rest;
    getline(linetokens, cmd, ' ');
    getline(linetokens, rest);
    if (cmd == "mod") {
      key.modulus = to_bin(rest);
    } else if (cmd == "blob") {
      key.blob = to_bin(rest);
    } else if (cmd == "exp") {
      key.exponent = to_bin(rest);
    } else {
      throw std::runtime_error("Keyfile format error(line=" + line + ")");
    }
  }
  if (key.modulus.empty() || key.blob.empty() || key.exponent.empty()) {
    throw std::runtime_error("Keyfile incomplete. TODO: better error.");
  }
  return key;
}

bool
auth_required(const std::string* srk_pin, const Key& key)
{
  TPMStuff stuff{srk_pin};

  int init_flags =
    TSS_KEY_TYPE_SIGNING
    | TSS_KEY_SIZE_2048
    | TSS_KEY_VOLATILE
    | TSS_KEY_NO_AUTHORIZATION
    | TSS_KEY_NOT_MIGRATABLE;

  TSS_HKEY hkey;
  TSCALL(Tspi_Context_CreateObject, stuff.ctx(), TSS_OBJECT_TYPE_RSAKEY,
         init_flags, &hkey);
  TSCALL(Tspi_Context_LoadKeyByBlob, stuff.ctx(), stuff.srk(),
         key.blob.size(), (BYTE*)key.blob.data(), &hkey);

  UINT32 auth;
  TSCALL(Tspi_GetAttribUint32, hkey,
         TSS_TSPATTRIB_KEY_INFO, TSS_TSPATTRIB_KEYINFO_AUTHDATAUSAGE,
         &auth);
  return !!auth;
}

std::string
sign(const Key& key, const std::string& data,
     const std::string* srk_pin,
     const std::string* key_pin)
{
  TPMStuff stuff{srk_pin};

  // === Load key ===
  int init_flags =
    TSS_KEY_TYPE_SIGNING
    | TSS_KEY_VOLATILE
    | TSS_KEY_NO_AUTHORIZATION
    | TSS_KEY_NOT_MIGRATABLE;
  TSS_HKEY sign;
  TSS_HPOLICY policy_sign;
  TSCALL(Tspi_Context_CreateObject, stuff.ctx(), TSS_OBJECT_TYPE_RSAKEY,
         init_flags, &sign);
  TSCALL(Tspi_Context_LoadKeyByBlob, stuff.ctx(), stuff.srk(),
         key.blob.size(), (BYTE*)key.blob.data(), &sign);
  TSCALL(Tspi_Context_CreateObject, stuff.ctx(),
         TSS_OBJECT_TYPE_POLICY, TSS_POLICY_USAGE,
         &policy_sign);

  if (key_pin) {
    TSCALL(Tspi_Policy_SetSecret, policy_sign,
           TSS_SECRET_MODE_PLAIN,
           key_pin->size(),
           (BYTE*)key_pin->data());
  } else {
    BYTE wks[] = TSS_WELL_KNOWN_SECRET;
    int wks_size = sizeof(wks);
    TSCALL(Tspi_Policy_SetSecret, policy_sign,
           TSS_SECRET_MODE_SHA1, wks_size, wks);
  }
  TSCALL(Tspi_Policy_AssignToObject, policy_sign, sign);

  // === Sign ===
  TSS_HHASH hash;
  UINT32 sig_size;
  BYTE* sig_blob;
  TSCALL(Tspi_Context_CreateObject, stuff.ctx(),
         TSS_OBJECT_TYPE_HASH, TSS_HASH_OTHER, &hash);
  TSCALL(Tspi_Hash_SetHashValue, hash, data.size(), (BYTE*)data.data());
  if (false) {
    TSCALL(Tspi_SetAttribUint32, sign, TSS_TSPATTRIB_KEY_INFO,
           TSS_TSPATTRIB_KEYINFO_SIGSCHEME,
           TSS_SS_RSASSAPKCS1V15_DER);
  }
  TSCALL(Tspi_Hash_Sign, hash, sign, &sig_size, &sig_blob);
  return std::string{sig_blob, sig_blob+sig_size};
}
END_NAMESPACE(stpm);
/* ---- Emacs Variables ----
 * Local Variables:
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * End:
 */

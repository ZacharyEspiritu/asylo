/*
 *
 * Copyright 2019 Asylo authors
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
 *
 */

#include "asylo/identity/sgx/remote_assertion_generator_enclave_util.h"

#include <google/protobuf/repeated_field.h>
#include "absl/strings/string_view.h"
#include "asylo/crypto/algorithms.pb.h"
#include "asylo/crypto/ecdsa_p256_sha256_signing_key.h"
#include "asylo/crypto/util/byte_container_util.h"
#include "asylo/crypto/util/trivial_object_util.h"
#include "asylo/grpc/auth/enclave_server_credentials.h"
#include "asylo/grpc/auth/sgx_local_credentials_options.h"
#include "asylo/identity/additional_authenticated_data_generator.h"
#include "asylo/identity/sealed_secret.pb.h"
#include "asylo/identity/sgx/attestation_key.pb.h"
#include "asylo/identity/sgx/remote_assertion_generator_enclave.pb.h"
#include "asylo/identity/sgx/sgx_local_secret_sealer.h"
#include "asylo/identity/sgx/sgx_remote_assertion_generator_impl.h"
#include "asylo/util/cleansing_types.h"
#include "asylo/util/status.h"
#include "asylo/util/status_macros.h"
#include "include/grpcpp/security/server_credentials.h"
#include "include/grpcpp/server_builder.h"

namespace asylo {
namespace sgx {

const char *const kAttestationPublicKeyVersion =
    "Assertion Generator Enclave Attestation Key v0.1";
const char *const kAttestationPublicKeyPurpose =
    "Assertion Generator Enclave Attestation Key";
const char *const kPceSignReportPayloadVersion = "PCE Sign Report v0.1";

namespace {

using ::google::protobuf::RepeatedPtrField;

constexpr char kSecretName[] = "Assertion Generator Enclave Secret";
constexpr char kSecretVersion[] = "Assertion Generator Enclave Secret v0.1";
constexpr char kSecretPurpose[] =
    "Assertion Generator Enclave Attestation Key and Certificates";

void SetAsymmetricSigningKeyProto(
    const std::string &serialized_key_der,
    AsymmetricSigningKeyProto_KeyType key_type,
    SignatureScheme signature_scheme,
    AsymmetricSigningKeyProto *asymmetric_signing_key_proto) {
  asymmetric_signing_key_proto->set_key(serialized_key_der);
  asymmetric_signing_key_proto->set_encoding(
      AsymmetricKeyEncoding::ASYMMETRIC_KEY_DER);
  asymmetric_signing_key_proto->set_key_type(key_type);
  asymmetric_signing_key_proto->set_signature_scheme(signature_scheme);
}

}  // namespace

Status CheckRemoteAssertionGeneratorEnclaveSecretHeader(
    const SealedSecretHeader &header) {
  if (header.secret_name() != kSecretName) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Invalid sealed secret header: incorrect secret name");
  }
  if (header.secret_version() != kSecretVersion) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Invalid sealed secret header: incorrect secret version");
  }
  if (header.secret_purpose() != kSecretPurpose) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Invalid sealed secret header: incorrect secret purpose");
  }
  return Status::OkStatus();
}

SealedSecretHeader GetRemoteAssertionGeneratorEnclaveSecretHeader() {
  SealedSecretHeader header;
  header.set_secret_name(kSecretName);
  header.set_secret_version(kSecretVersion);
  header.set_secret_purpose(kSecretPurpose);
  return header;
}

StatusOr<SealedSecret> CreateSealedSecret(
    const SealedSecretHeader &header,
    const RepeatedPtrField<CertificateChain> &certificate_chains,
    const SigningKey &attestation_key) {
  std::unique_ptr<SgxLocalSecretSealer> sealer =
      SgxLocalSecretSealer::CreateMrsignerSecretSealer();

  SealedSecretHeader secret_header;
  sealer->SetDefaultHeader(&secret_header);
  secret_header.MergeFrom(header);

  RemoteAssertionGeneratorEnclaveSecret enclave_secret;
  ASYLO_ASSIGN_OR_RETURN(
      *enclave_secret.mutable_attestation_key(),
      GetAsymmetricSigningKeyProtoFromSigningKey(attestation_key));

  RemoteAssertionGeneratorEnclaveSecretAad aad;
  *aad.mutable_certificate_chains() = certificate_chains;

  std::string serialized_enclave_secret = enclave_secret.SerializeAsString();
  if (serialized_enclave_secret.empty()) {
    return Status(error::GoogleError::INTERNAL,
                  "Enclave secret serialization failed");
  }
  std::string serialized_aad = aad.SerializeAsString();
  if (serialized_aad.empty()) {
    return Status(error::GoogleError::INTERNAL,
                  "Enclave additional authenticated data serialization failed");
  }
  SealedSecret sealed_secret;
  ASYLO_RETURN_IF_ERROR(sealer->Seal(secret_header, serialized_aad,
                                     serialized_enclave_secret,
                                     &sealed_secret));
  return sealed_secret;
}

StatusOr<std::unique_ptr<EcdsaP256Sha256SigningKey>>
ExtractAttestationKeyAndCertificateChainsFromSealedSecret(
    const SealedSecret &sealed_secret,
    std::vector<CertificateChain> *certificate_chains) {
  SealedSecretHeader header;
  if (!header.ParseFromString(sealed_secret.sealed_secret_header())) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Cannot parse the sealed secret header");
  }
  ASYLO_RETURN_IF_ERROR(
      CheckRemoteAssertionGeneratorEnclaveSecretHeader(header));

  CleansingVector<uint8_t> serialized_secret;
  std::unique_ptr<SgxLocalSecretSealer> sealer =
      SgxLocalSecretSealer::CreateMrsignerSecretSealer();
  ASYLO_RETURN_IF_ERROR(sealer->Unseal(sealed_secret, &serialized_secret));
  RemoteAssertionGeneratorEnclaveSecret enclave_secret;
  if (!enclave_secret.ParseFromArray(serialized_secret.data(),
                                     serialized_secret.size())) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Cannot parse the sealed secret");
  }

  RemoteAssertionGeneratorEnclaveSecretAad aad;
  if (!aad.ParseFromString(sealed_secret.additional_authenticated_data())) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Cannot parse the additional authenticated data");
  }
  *certificate_chains = {aad.certificate_chains().cbegin(),
                         aad.certificate_chains().cend()};

  return ExtractAttestationKeyFromAsymmetricSigningKeyProto(
      enclave_secret.attestation_key());
}

StatusOr<std::unique_ptr<EcdsaP256Sha256SigningKey>>
ExtractAttestationKeyFromAsymmetricSigningKeyProto(
    const AsymmetricSigningKeyProto &asymmetric_signing_key_proto) {
  if (asymmetric_signing_key_proto.key_type() !=
      AsymmetricSigningKeyProto::SIGNING_KEY) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  absl::StrCat("The sealed secret key has invalid key type: ",
                               AsymmetricSigningKeyProto_KeyType_Name(
                                   asymmetric_signing_key_proto.key_type())));
  }
  switch (asymmetric_signing_key_proto.encoding()) {
    case AsymmetricKeyEncoding::ASYMMETRIC_KEY_DER:
      return EcdsaP256Sha256SigningKey::CreateFromDer(
          {asymmetric_signing_key_proto.key()});
    case AsymmetricKeyEncoding::ASYMMETRIC_KEY_PEM:
      return Status(
          error::GoogleError::UNIMPLEMENTED,
          "Create attestation key from a PEM-encoded key is not supported");
    default:
      return Status(error::GoogleError::INVALID_ARGUMENT,
                    "AsymmetricSigningKeyProto has unknown encoding format");
  }
}

StatusOr<AsymmetricSigningKeyProto> GetAsymmetricSigningKeyProtoFromSigningKey(
    const SigningKey &signing_key) {
  CleansingVector<uint8_t> signing_key_der;
  ASYLO_RETURN_IF_ERROR(signing_key.SerializeToDer(&signing_key_der));
  std::string serialized_key_der = {signing_key_der.begin(),
                                    signing_key_der.end()};

  AsymmetricSigningKeyProto asymmetric_signing_key_proto;
  SetAsymmetricSigningKeyProto(
      serialized_key_der, AsymmetricSigningKeyProto::SIGNING_KEY,
      signing_key.GetSignatureScheme(), &asymmetric_signing_key_proto);
  return asymmetric_signing_key_proto;
}

StatusOr<std::string> CreateSerializedPceSignReportPayloadFromVerifyingKey(
    const VerifyingKey &verifying_key) {
  std::string serialized_key_der;
  ASYLO_ASSIGN_OR_RETURN(serialized_key_der, verifying_key.SerializeToDer());

  PceSignReportPayload pce_sign_report_payload;
  pce_sign_report_payload.set_version(kPceSignReportPayloadVersion);
  AttestationPublicKey *public_key =
      pce_sign_report_payload.mutable_attestation_public_key();
  SetAsymmetricSigningKeyProto(serialized_key_der,
                               AsymmetricSigningKeyProto::VERIFYING_KEY,
                               verifying_key.GetSignatureScheme(),
                               public_key->mutable_attestation_public_key());
  public_key->set_version(kAttestationPublicKeyVersion);
  public_key->set_purpose(kAttestationPublicKeyPurpose);
  return pce_sign_report_payload.SerializeAsString();
}

StatusOr<Reportdata> GenerateReportdataForPceSignReportProtocol(
    absl::string_view serialized_pce_sign_report_payload) {
  std::unique_ptr<AdditionalAuthenticatedDataGenerator> aad_generator;
  ASYLO_ASSIGN_OR_RETURN(
      aad_generator,
      AdditionalAuthenticatedDataGenerator::CreatePceSignReportAadGenerator());

  std::string aad_data;
  ASYLO_ASSIGN_OR_RETURN(
      aad_data, aad_generator->Generate(serialized_pce_sign_report_payload));

  Reportdata reportdata;
  ASYLO_RETURN_IF_ERROR(
      SetTrivialObjectFromBinaryString(aad_data, &reportdata));
  return reportdata;
}

StatusOr<std::unique_ptr<::grpc::Server>> CreateAndStartServer(
    std::string remote_assertion_generator_server_address,
    SgxRemoteAssertionGeneratorImpl *remote_assertion_generator_service) {
  ::grpc::ServerBuilder builder;
  builder.RegisterService(remote_assertion_generator_service);
  // Enforce authentication based on SGX-local attestation.
  std::shared_ptr<::grpc::ServerCredentials> credentials =
      EnclaveServerCredentials(BidirectionalSgxLocalCredentialsOptions());
  builder.AddListeningPort(remote_assertion_generator_server_address,
                           credentials);
  std::unique_ptr<::grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    return Status(error::GoogleError::INTERNAL, "Failed to start server");
  }

  LOG(INFO) << "RemoteAssertionGenerator server started at address: "
            << remote_assertion_generator_server_address;

  return std::move(server);
}

}  // namespace sgx
}  // namespace asylo

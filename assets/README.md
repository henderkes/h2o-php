# Embedded CA data

`ca-bundle.pem` is the official curl export of Mozilla's public CA certificates,
retrieved from <https://curl.se/ca/cacert.pem> on 2026-11-03. The file records
Mozilla data dated 2026-10-20 and contains 118 certificates. File SHA256:

`796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe`

The repository stores this exact input. Normal builds never fetch a newer trust
store. Review and update the recorded digest in `cmake/EmbedAssets.cmake` when
refreshing it. Upstream extraction/provenance documentation:
<https://curl.se/docs/caextract.html>. Mozilla source:
<https://github.com/mozilla-firefox/firefox/blob/release/security/nss/lib/ckfw/builtins/certdata.txt>.

The data is distributed under MPL 2.0; the unmodified official license is included
as `LICENSE.MPL-2.0`. The PEM export does not carry all browser-specific certificate
policy constraints. The runtime honors explicit CA configuration and prefers
available platform trust for PHP's default OpenSSL client configuration.

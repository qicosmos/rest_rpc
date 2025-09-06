#!/bin/bash
# generate_cert.sh

# 生成私钥
openssl genrsa -out server.key 2048

# 生成证书签名请求 (CSR)
openssl req -new -key server.key -out server.csr -subj "/CN=localhost"

# 生成自签名证书 (CRT)
openssl x509 -req -days 365 -in server.csr -signkey server.key -out server.crt

# 清理临时文件
rm server.csr

echo "证书生成完成！"
echo "server.key - 私钥（保密）"
echo "server.crt - 证书（公开）"
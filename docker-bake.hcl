variable "IMAGE" {
  default = "ghcr.io/timoschenko/komodo-telegram-alerter"
}

variable "TAG" {
  default = "latest"
}

target "default" {
  dockerfile = "Dockerfile"
  context    = "."
  platforms  = ["linux/amd64", "linux/arm64", "linux/arm/v7"]
  tags       = ["${IMAGE}:${TAG}"]
}

target "rpi" {
  dockerfile = "Dockerfile"
  context    = "."
  platforms  = ["linux/arm64", "linux/arm/v7", "linux/arm/v6"]
  tags       = ["${IMAGE}:${TAG}"]
}

target "rpi64" {
  dockerfile = "Dockerfile"
  context    = "."
  platforms  = ["linux/arm64"]
  tags       = ["${IMAGE}:${TAG}-arm64"]
}

target "rpi32" {
  dockerfile = "Dockerfile"
  context    = "."
  platforms  = ["linux/arm/v7"]
  tags       = ["${IMAGE}:${TAG}-armv7"]
}

target "rpi-zero" {
  dockerfile = "Dockerfile"
  context    = "."
  platforms  = ["linux/arm/v6"]
  tags       = ["${IMAGE}:${TAG}-armv6"]
}

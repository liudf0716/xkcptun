#
# Copyright (C) 2014-2017 ZengFei Zhang <zhangzengfei@kunteng.org>
#
# This is free software, licensed under the GNU General Public License v3.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=xkcptun
PKG_VERSION:=0.4.266
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/liudf0716/xkcptun/releases/download/$(PKG_VERSION)/
PKG_MD5SUM:=865555c97e67bc818544ce331eb96c2e

PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/$(PKG_NAME)/Default
	SECTION:=net
	CATEGORY:=Network
	TITLE:=A Secure Tunnel Based On KCP with N:M Multiplexing
	URL:=https://github.com/liudf0716/xkcptun
	DEPENDS:=+libjson-c +libevent2
endef

Package/$(PKG_NAME) = $(Package/$(PKG_NAME)/Default)
Package/$(PKG_NAME)-server = $(Package/$(PKG_NAME)/Default)

define Package/$(PKG_NAME)/description
Xkcptun is a secure tunnel based on KCP implemented in the C language. for embedded devices and low end boxes.
endef

Package/$(PKG_NAME)-server/description = $(Package/$(PKG_NAME)/description)

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xkcp_client $(1)/usr/bin
endef

define Package/$(PKG_NAME)-server/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xkcp_server $(1)/usr/bin
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
$(eval $(call BuildPackage,$(PKG_NAME)-server))

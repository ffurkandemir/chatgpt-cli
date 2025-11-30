# ChatGPT CLI (C Edition) - Makefile

# Derleyici ve bayraklar
CC      := gcc
CFLAGS  := -Wall -Wextra -O2
LDFLAGS := -lcurl

# Binary adı
BIN     := chatgpt

# Kaynak dosya
SRC     := chatgpt.c

# Kurulum yolu (varsayılan: ~/.local/bin)
PREFIX  := $(HOME)/.local
BINDIR  := $(PREFIX)/bin

# Versiyon (VERSION dosyasından okunur)
VERSION := $(shell cat VERSION 2>/dev/null || echo "v0.0.0")

.PHONY: all clean install uninstall release

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

clean:
	rm -f $(BIN) chatgpt-linux-x86_64.tar.gz

install: $(BIN)
	mkdir -p $(BINDIR)
	cp $(BIN) $(BINDIR)/$(BIN)
	@echo "Installed to $(BINDIR)/$(BIN)"

uninstall:
	rm -f $(BINDIR)/$(BIN)
	@echo "Removed $(BINDIR)/$(BIN)"

# Basit bir release arşivi oluşturur (Linux x86_64)
release: $(BIN)
	@echo "Creating release archive for $(VERSION)..."
	tar -czvf chatgpt-linux-x86_64-$(VERSION).tar.gz $(BIN)
	@echo "Archive created: chatgpt-linux-x86_64-$(VERSION).tar.gz"

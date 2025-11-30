# ChatGPT CLI (C Implementation)

A lightweight, robust, and feature-rich Command Line Interface for OpenAI's ChatGPT, written in pure C. Designed for efficiency, security, and ease of use in Linux environments.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Language](https://img.shields.io/badge/language-C-green.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)

## 🚀 Features

- **⚡ Ultra Lightweight**: Written in standard C using `libcurl`. Minimal resource footprint.
- **🧠 Context Awareness**: Remembers your conversation history in interactive mode for natural dialogue.
- **🌍 Multi-Language UI**: Fully localized interface supporting **English** and **Turkish** (auto-detected or user-selected).
- **💾 Persistent Configuration**: Automatically saves your API key, preferred model, and language settings to `~/.config/chatgpt-cli/`.
- **🎯 One-Shot Mode**: Execute single queries directly from the command line arguments with a minimalist output.
- **🛡️ Robust & Secure**: Implements safe memory management, SSL verification, and secure file handling.
- **🔧 Developer Friendly**: Includes helper commands to run shell snippets suggested by the AI.

## 📦 Installation

### Prerequisites
You need `gcc` and `libcurl` installed on your system.

**Debian/Ubuntu:**
```bash
sudo apt-get update
sudo apt-get install build-essential libcurl4-openssl-dev
```

**Fedora/RHEL:**
```bash
sudo dnf install gcc libcurl-devel
```

### Build & Install

1. Clone the repository:
   ```bash
   git clone https://github.com/ffurkandemir/chatgpt-cli.git
   cd chatgpt-cli
   ```

2. Compile and install using Makefile:
   ```bash
   make
   sudo make install
   ```

3. Verify installation:
   ```bash
   chatgpt --help
   ```

## 📖 Usage

### 1. Interactive Mode
Start the conversation loop by running the command without arguments.
```bash
chatgpt
```
*On the first run, it will ask for your OpenAI API Key and preferred language.*

**Commands inside interactive mode:**
- `/history`: Show conversation history.
- `/clear`: Clear conversation context.
- `/model`: Show current active model.
- `/ml` or `/multi`: Enter multi-line input mode (end with a `.` on a new line).
- `/run N`: Execute the Nth code block/command suggested by ChatGPT in the last response.
- `/exit`: Quit the application.

### 2. One-Shot Mode
Ask a quick question and get the answer immediately. Perfect for scripting or quick lookups.
```bash
chatgpt "How do I recursively find files in Linux?"
```

### 3. Configuration Options
You can pass flags to override defaults:

```bash
# Use a specific model for this session
chatgpt --model gpt-4 "Explain quantum physics"

# Set a new default model permanently
chatgpt --set-default-model gpt-4o

# List available models (hardcoded examples)
chatgpt --list-models
```

## ⚙️ Configuration File
Configuration is stored in `~/.config/chatgpt-cli/`.
- `config`: Stores your API Key.
- `model`: Stores your default model preference.
- `lang`: Stores your language preference (`tr` or `en`).

## 🤝 Contributing
Contributions are welcome! Please feel free to submit a Pull Request.

1. Fork the project
2. Create your feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

## 📄 License
Distributed under the MIT License. See `LICENSE` for more information.

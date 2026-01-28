# GitHub 上传指南

## 步骤 1: 配置 Git 用户信息（如果还没有配置）

```bash
git config --global user.name "Your Name"
git config --global user.email "your.email@example.com"
```

或者仅为当前仓库设置：

```bash
cd /Users/shichaochen/cursor/WaterQualityMonitor-BJ
git config user.name "Your Name"
git config user.email "your.email@example.com"
```

## 步骤 2: 提交代码

```bash
cd /Users/shichaochen/cursor/WaterQualityMonitor-BJ
git add .
git commit -m "Initial commit: ESP32 水质监测系统 with 多组WiFi配置和自动重连功能"
```

## 步骤 3: 在 GitHub 上创建新仓库

1. 登录 GitHub
2. 点击右上角的 "+" 号，选择 "New repository"
3. 输入仓库名称（例如：`WaterQualityMonitor-BJ`）
4. 选择 Public 或 Private
5. **不要**勾选 "Initialize this repository with a README"（因为我们已经有了）
6. 点击 "Create repository"

## 步骤 4: 连接本地仓库到 GitHub

GitHub 会显示仓库的 URL，使用以下命令连接：

```bash
cd /Users/shichaochen/cursor/WaterQualityMonitor-BJ
git remote add origin https://github.com/YOUR_USERNAME/WaterQualityMonitor-BJ.git
```

（将 `YOUR_USERNAME` 替换为你的 GitHub 用户名）

## 步骤 5: 推送代码到 GitHub

```bash
git branch -M main
git push -u origin main
```

如果使用 SSH（推荐）：

```bash
git remote add origin git@github.com:YOUR_USERNAME/WaterQualityMonitor-BJ.git
git branch -M main
git push -u origin main
```

## 后续更新代码

当你修改代码后，使用以下命令更新 GitHub：

```bash
git add .
git commit -m "描述你的更改"
git push
```

## 注意事项

- 确保 `.gitignore` 文件已正确配置，避免上传编译文件
- 不要上传敏感信息（如 API Key、密码等）
- 如果代码中包含敏感信息，请先移除或使用环境变量


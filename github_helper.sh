#!/bin/bash

# GitHub仓库信息
GITHUB_USER="yuhua2025"
REPO_NAME="xiaozhi-esp32-dayustar"
REPO_URL="https://github.com/${GITHUB_USER}/${REPO_NAME}.git"

# 项目信息
PROJECT_NAME="xiaozhi-esp32-dayustar"
BUNDLE_FILE="xiaozhi-esp32-dayustar.bundle"

# 输出颜色设置
GREEN="\033[0;32m"
YELLOW="\033[1;33m"
RED="\033[0;31m"
NC="\033[0m" # No Color

echo -e "${GREEN}=== 小直ESP32项目GitHub上传助手 ===${NC}"
echo

# 检查git是否安装
if ! command -v git &> /dev/null; then
    echo -e "${RED}错误: git 命令未找到，请先安装git${NC}"
    exit 1
fi

# 检查curl是否安装
if ! command -v curl &> /dev/null; then
    echo -e "${RED}错误: curl 命令未找到，请先安装curl${NC}"
    exit 1
fi

# 检查bundle文件是否存在
if [ ! -f "$BUNDLE_FILE" ]; then
    echo -e "${YELLOW}警告: 未找到bundle文件，正在创建...${NC}"
    git bundle create "$BUNDLE_FILE" --all
    if [ $? -ne 0 ]; then
        echo -e "${RED}错误: 创建bundle文件失败${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓ Bundle文件创建成功${NC}"
fi

# 检查GitHub CLI是否安装
if command -v gh &> /dev/null; then
    echo -e "${GREEN}✓ GitHub CLI已安装${NC}"
    
    # 检查是否已登录GitHub
    if gh auth status &> /dev/null; then
        echo -e "${GREEN}✓ 已登录GitHub${NC}"
        
        # 检查仓库是否存在
        if gh repo view "${GITHUB_USER}/${REPO_NAME}" &> /dev/null; then
            echo -e "${GREEN}✓ GitHub仓库已存在${NC}"
        else
            echo -e "${YELLOW}GitHub仓库不存在，是否创建？(y/n)${NC}"
            read -r create_repo
            if [[ "$create_repo" == "y" || "$create_repo" == "Y" ]]; then
                gh repo create "${REPO_NAME}" --public --description "小直ESP32项目 - Dayu Star WiFi支持" --enable-wiki=false
                if [ $? -eq 0 ]; then
                    echo -e "${GREEN}✓ GitHub仓库创建成功${NC}"
                else
                    echo -e "${RED}错误: 创建GitHub仓库失败${NC}"
                fi
            fi
        fi
    else
        echo -e "${YELLOW}需要登录GitHub CLI，请运行: gh auth login${NC}"
    fi
else
    echo -e "${YELLOW}GitHub CLI未安装，建议安装以简化操作${NC}"
    echo -e "${YELLOW}安装命令: brew install gh 或 参考 https://cli.github.com/${NC}"
fi

echo
echo -e "${GREEN}=== 解决方案列表 ===${NC}"
echo "1. 方案一: 使用git bundle (推荐)"
echo "2. 方案二: 手动创建仓库并推送"
echo "3. 方案三: 检查网络连接"
echo

echo -e "${GREEN}=== 方案一使用说明 ===${NC}"
echo "1. 将生成的 $BUNDLE_FILE 文件下载到本地"
echo "2. 在目标目录运行: git clone $BUNDLE_FILE $PROJECT_NAME"
echo "3. cd $PROJECT_NAME"
echo "4. git remote add origin $REPO_URL"
echo "5. git push -u origin --all"
echo

echo -e "${GREEN}=== 方案二使用说明 ===${NC}"
echo "1. 手动在GitHub创建仓库: $REPO_URL"
echo "2. 确保仓库为空（不勾选初始化README等选项）"
echo "3. 在项目目录运行:"
echo "   git remote add github $REPO_URL"
echo "   git push github main"
echo

echo -e "${GREEN}=== 网络连接检查 ===${NC}"
echo "检查GitHub连接..."
if ping -c 1 github.com &> /dev/null; then
    echo -e "${GREEN}✓ GitHub可以ping通${NC}"
else
    echo -e "${RED}✗ 无法ping通GitHub，请检查网络连接${NC}"
fi

echo "检查HTTPS连接..."
if curl -s -o /dev/null -w "%{http_code}" https://github.com > /dev/null; then
    echo -e "${GREEN}✓ HTTPS连接正常${NC}"
else
    echo -e "${RED}✗ HTTPS连接失败，请检查防火墙或代理设置${NC}"
fi

echo
echo -e "${GREEN}=== 项目信息 ===${NC}"
echo "项目目录: $(pwd)"
echo "Git状态:"
git status --short
echo "Bundle文件大小: $(du -h $BUNDLE_FILE 2>/dev/null | cut -f1)"
echo
echo -e "${YELLOW}提示: 如果遇到认证问题，可以使用SSH方式或GitHub Desktop工具${NC}"
echo -e "${YELLOW}SSH URL: git@github.com:${GITHUB_USER}/${REPO_NAME}.git${NC}"

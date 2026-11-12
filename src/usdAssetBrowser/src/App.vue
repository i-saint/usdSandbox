<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import Button from 'primevue/button'
import IconField from 'primevue/iconfield'
import InputIcon from 'primevue/inputicon'
import InputText from 'primevue/inputtext'
import Message from 'primevue/message'
import ProgressSpinner from 'primevue/progressspinner'
import Slider from 'primevue/slider'
import Splitter from 'primevue/splitter'
import SplitterPanel from 'primevue/splitterpanel'
import Tree from 'primevue/tree'
import type { TreeExpandedKeys, TreeSelectionKeys } from 'primevue/tree'
import type { TreeNode } from 'primevue/treenode'

import AssetCard from './components/AssetCard.vue'
import {
  buildDirectoryTree,
  buildScreenshotUrl,
  dirToTreeKey,
  fetchDirectories,
  fetchDirectoryList,
  normalizeBaseUrl,
  resolveInitialServerUrl,
  saveServerUrl,
} from './api'

const serverUrlInput = ref(resolveInitialServerUrl())
const activeServerUrl = ref(serverUrlInput.value)

const treeNodes = ref<TreeNode[]>([])
const selectionKeys = ref<TreeSelectionKeys>({})
const expandedKeys = ref<TreeExpandedKeys>({ '/': true })

const currentDir = ref('')
const subDirectories = ref<string[]>([])
const files = ref<string[]>([])
const searchQuery = ref('')
const cardWidth = ref(200)
const reloadToken = ref(0)

const loadingDirs = ref(false)
const loadingList = ref(false)
const errorMessage = ref('')

const breadcrumbs = computed(() => {
  const items: Array<{ label: string; dir: string }> = [{ label: '/', dir: '' }]
  if (!currentDir.value) {
    return items
  }
  const parts = currentDir.value.split('/').filter(Boolean)
  let acc = ''
  for (const part of parts) {
    acc = acc ? `${acc}/${part}` : part
    items.push({ label: part, dir: acc })
  }
  return items
})

const filteredSubDirectories = computed(() => {
  const q = searchQuery.value.trim().toLowerCase()
  if (!q) return subDirectories.value
  return subDirectories.value.filter((d) => d.toLowerCase().includes(q))
})

const filteredFiles = computed(() => {
  const q = searchQuery.value.trim().toLowerCase()
  if (!q) return files.value
  return files.value.filter((f) => f.toLowerCase().includes(q))
})

function expandAncestors(dir: string) {
  const next: TreeExpandedKeys = { ...expandedKeys.value, '/': true }
  if (dir) {
    const parts = dir.split('/').filter(Boolean)
    let acc = ''
    for (const part of parts) {
      acc = acc ? `${acc}/${part}` : part
      next[dirToTreeKey(acc)] = true
    }
  }
  expandedKeys.value = next
}

async function selectDirectory(dir: string) {
  currentDir.value = dir
  const key = dirToTreeKey(dir)
  selectionKeys.value = { [key]: true }
  expandAncestors(dir)

  loadingList.value = true
  errorMessage.value = ''
  try {
    const list = await fetchDirectoryList(activeServerUrl.value, dir)
    subDirectories.value = list.directories ?? []
    files.value = list.files ?? []
  } catch (err) {
    subDirectories.value = []
    files.value = []
    errorMessage.value = err instanceof Error ? err.message : String(err)
  } finally {
    loadingList.value = false
  }
}

async function loadFromServer(preserveCurrentDir = false) {
  const normalized = normalizeBaseUrl(serverUrlInput.value)
  if (!normalized) return

  activeServerUrl.value = normalized
  serverUrlInput.value = normalized
  saveServerUrl(normalized)
  reloadToken.value = Date.now()

  loadingDirs.value = true
  errorMessage.value = ''
  try {
    const dirs = await fetchDirectories(normalized)
    treeNodes.value = buildDirectoryTree(dirs)

    if (dirs.length === 0) {
      currentDir.value = ''
      subDirectories.value = []
      files.value = []
      selectionKeys.value = {}
      return
    }

    const targetDir =
      preserveCurrentDir && dirs.includes(currentDir.value) ? currentDir.value : dirs[0]!
    await selectDirectory(targetDir)
  } catch (err) {
    treeNodes.value = []
    subDirectories.value = []
    files.value = []
    errorMessage.value = err instanceof Error ? err.message : String(err)
  } finally {
    loadingDirs.value = false
  }
}

function onNodeSelect(node: TreeNode) {
  const dir = typeof node.data === 'string' ? node.data : ''
  void selectDirectory(dir)
}

function openSubDirectory(subDirName: string) {
  const nextDir = currentDir.value ? `${currentDir.value}/${subDirName}` : subDirName
  void selectDirectory(nextDir)
}

function getScreenshotUrl(fileName: string): string {
  return buildScreenshotUrl(
    activeServerUrl.value,
    currentDir.value,
    fileName,
    reloadToken.value,
  )
}

onMounted(() => {
  void loadFromServer(false)
})
</script>

<template>
  <div class="browser-shell">
    <header class="topbar">
      <div class="brand">
        <i class="pi pi-box brand-icon" />
        <span class="brand-title">USD Asset Browser</span>
      </div>

      <form class="server-form" @submit.prevent="loadFromServer(true)">
        <InputText
          v-model="serverUrlInput"
          placeholder="http://127.0.0.1:8080"
          size="small"
          class="server-input"
        />
        <Button
          type="submit"
          icon="pi pi-refresh"
          label="更新"
          size="small"
          severity="secondary"
          :loading="loadingDirs || loadingList"
        />
      </form>

      <div class="topbar-right">
        <IconField>
          <InputIcon class="pi pi-search" />
          <InputText
            v-model="searchQuery"
            placeholder="ファイル検索..."
            size="small"
            class="search-input"
          />
        </IconField>

        <div class="size-control" title="サムネイルサイズ">
          <i class="pi pi-th-large" />
          <Slider v-model="cardWidth" :min="140" :max="320" :step="10" class="size-slider" />
        </div>
      </div>
    </header>

    <Splitter class="main-splitter">
      <SplitterPanel :size="24" :minSize="15" class="tree-panel">
        <div class="panel-header">ディレクトリ</div>
        <div class="tree-container">
          <Tree
            v-if="treeNodes.length > 0"
            :value="treeNodes"
            v-model:selectionKeys="selectionKeys"
            v-model:expandedKeys="expandedKeys"
            selectionMode="single"
            :filter="true"
            filterMode="lenient"
            filterPlaceholder="フォルダ検索..."
            class="dir-tree"
            @node-select="onNodeSelect"
          />
          <div v-else-if="!loadingDirs" class="empty-hint">
            ディレクトリがありません
          </div>
        </div>
      </SplitterPanel>

      <SplitterPanel :size="76" :minSize="40" class="content-panel">
        <div class="breadcrumb-bar">
          <div class="breadcrumbs">
            <template v-for="(crumb, idx) in breadcrumbs" :key="crumb.dir">
              <span v-if="idx > 0" class="crumb-sep">/</span>
              <button
                type="button"
                class="crumb-btn"
                :class="{ active: idx === breadcrumbs.length - 1 }"
                @click="selectDirectory(crumb.dir)"
              >
                {{ crumb.label }}
              </button>
            </template>
          </div>
          <span class="item-count">
            {{ filteredFiles.length }} ファイル
            <template v-if="filteredSubDirectories.length > 0">
              / {{ filteredSubDirectories.length }} フォルダ
            </template>
          </span>
        </div>

        <div class="content-scroll">
          <Message v-if="errorMessage" severity="error" :closable="false" class="error-msg">
            サーバー ({{ activeServerUrl }}) からの取得に失敗しました: {{ errorMessage }}
          </Message>

          <div v-if="loadingList" class="loading-center">
            <ProgressSpinner style="width: 40px; height: 40px" strokeWidth="4" />
          </div>

          <template v-else>
            <div v-if="filteredSubDirectories.length > 0" class="subdir-grid">
              <button
                v-for="subDir in filteredSubDirectories"
                :key="subDir"
                type="button"
                class="subdir-card"
                @click="openSubDirectory(subDir)"
              >
                <i class="pi pi-folder subdir-icon" />
                <span class="subdir-name">{{ subDir }}</span>
              </button>
            </div>

            <div v-if="filteredFiles.length > 0" class="asset-grid">
              <AssetCard
                v-for="fileName in filteredFiles"
                :key="`${currentDir}/${fileName}`"
                :fileName="fileName"
                :screenshotUrl="getScreenshotUrl(fileName)"
                :cardWidth="cardWidth"
              />
            </div>

            <div
              v-if="
                !errorMessage &&
                filteredSubDirectories.length === 0 &&
                filteredFiles.length === 0
              "
              class="empty-hint"
            >
              表示できるアセットがありません
            </div>
          </template>
        </div>
      </SplitterPanel>
    </Splitter>
  </div>
</template>

<style scoped>
.browser-shell {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 100%;
}

.topbar {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 8px 16px;
  background: #181b20;
  border-bottom: 1px solid #282d35;
  flex-wrap: wrap;
}

.brand {
  display: flex;
  align-items: center;
  gap: 8px;
  font-weight: 600;
  font-size: 0.98rem;
  color: #f3f4f6;
}

.brand-icon {
  color: #60a5fa;
  font-size: 1.1rem;
}

.server-form {
  display: flex;
  align-items: center;
  gap: 8px;
}

.server-input {
  width: 240px;
}

.topbar-right {
  margin-left: auto;
  display: flex;
  align-items: center;
  gap: 16px;
}

.search-input {
  width: 200px;
}

.size-control {
  display: flex;
  align-items: center;
  gap: 10px;
  color: #9ca3af;
}

.size-slider {
  width: 100px;
}

.main-splitter {
  flex: 1;
  min-height: 0;
  border: none;
  border-radius: 0;
  background: transparent;
}

.tree-panel {
  display: flex;
  flex-direction: column;
  min-height: 0;
  background: #15181c;
}

.panel-header {
  padding: 10px 14px;
  font-size: 0.8rem;
  font-weight: 600;
  color: #9ca3af;
  border-bottom: 1px solid #242830;
  letter-spacing: 0.03em;
}

.tree-container {
  flex: 1;
  overflow: auto;
  padding: 6px;
}

.dir-tree {
  background: transparent;
  border: none;
  padding: 0;
}

.content-panel {
  display: flex;
  flex-direction: column;
  min-height: 0;
  background: #121417;
}

.breadcrumb-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 16px;
  background: #16191e;
  border-bottom: 1px solid #242830;
  gap: 12px;
}

.breadcrumbs {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 4px;
}

.crumb-sep {
  color: #6b7280;
  font-size: 0.85rem;
}

.crumb-btn {
  background: transparent;
  border: none;
  color: #93c5fd;
  font-size: 0.86rem;
  padding: 2px 6px;
  border-radius: 4px;
  cursor: pointer;
}

.crumb-btn:hover {
  background: #232831;
}

.crumb-btn.active {
  color: #f3f4f6;
  font-weight: 600;
}

.item-count {
  font-size: 0.8rem;
  color: #9ca3af;
  white-space: nowrap;
}

.content-scroll {
  flex: 1;
  overflow: auto;
  padding: 16px;
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.error-msg {
  margin: 0;
}

.loading-center {
  display: flex;
  justify-content: center;
  padding: 48px 0;
}

.subdir-grid {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
}

.subdir-card {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  background: #1b1f26;
  border: 1px solid #2b313b;
  border-radius: 6px;
  color: #e5e7eb;
  font-size: 0.86rem;
  cursor: pointer;
  max-width: 240px;
  transition: border-color 0.15s ease;
}

.subdir-card:hover {
  border-color: #60a5fa;
}

.subdir-icon {
  color: #fbbf24;
}

.subdir-name {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.asset-grid {
  display: flex;
  flex-wrap: wrap;
  gap: 14px;
  align-items: flex-start;
}

.empty-hint {
  color: #6b7280;
  font-size: 0.88rem;
  text-align: center;
  padding: 32px 12px;
}
</style>

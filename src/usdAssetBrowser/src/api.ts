import type { TreeNode } from 'primevue/treenode'

export interface DirectoriesResponse {
  directories: string[]
}

export interface DirectoryListResponse {
  dir: string
  directories: string[]
  files: string[]
}

const STORAGE_KEY_SERVER_URL = 'usdAssetBrowser.serverUrl'

export function normalizeBaseUrl(rawUrl: string): string {
  return rawUrl.trim().replace(/\/+$/, '')
}

export function resolveInitialServerUrl(): string {
  const params = new URLSearchParams(window.location.search)
  const fromQuery = params.get('server')
  if (fromQuery && fromQuery.trim()) {
    return normalizeBaseUrl(fromQuery)
  }

  const fromStorage = localStorage.getItem(STORAGE_KEY_SERVER_URL)
  if (fromStorage && fromStorage.trim()) {
    return normalizeBaseUrl(fromStorage)
  }

  if (
    window.location.protocol.startsWith('http') &&
    window.location.port !== '5173' &&
    window.location.port !== '4173'
  ) {
    return normalizeBaseUrl(window.location.origin)
  }

  return 'http://127.0.0.1:8080'
}

export function saveServerUrl(url: string): void {
  localStorage.setItem(STORAGE_KEY_SERVER_URL, normalizeBaseUrl(url))
}

export async function fetchDirectories(baseUrl: string): Promise<string[]> {
  const base = normalizeBaseUrl(baseUrl)
  const res = await fetch(`${base}/dirs`)
  if (!res.ok) {
    throw new Error(`GET /dirs failed (${res.status})`)
  }
  const data = (await res.json()) as DirectoriesResponse
  return Array.isArray(data.directories) ? data.directories : []
}

export async function fetchDirectoryList(
  baseUrl: string,
  dir: string,
): Promise<DirectoryListResponse> {
  const base = normalizeBaseUrl(baseUrl)
  const query = dir ? `?dir=${encodeURIComponent(dir)}` : ''
  const res = await fetch(`${base}/list${query}`)
  if (!res.ok) {
    throw new Error(`GET /list failed (${res.status})`)
  }
  return (await res.json()) as DirectoryListResponse
}

export function buildScreenshotUrl(
  baseUrl: string,
  dir: string,
  fileName: string,
  reloadToken = 0,
): string {
  const base = normalizeBaseUrl(baseUrl)
  const segments = dir ? dir.split('/').filter(Boolean).map(encodeURIComponent) : []
  segments.push(encodeURIComponent(`${fileName}.jpg`))
  const url = `${base}/${segments.join('/')}`
  return reloadToken > 0 ? `${url}?r=${reloadToken}` : url
}

export function dirToTreeKey(dir: string): string {
  return dir === '' ? '/' : `/${dir}`
}

export function buildDirectoryTree(directories: string[]): TreeNode[] {
  if (directories.length === 0) {
    return []
  }

  const nodeMap = new Map<string, TreeNode>()

  const ensureNode = (relDir: string): TreeNode => {
    const existing = nodeMap.get(relDir)
    if (existing) {
      return existing
    }

    const slashIndex = relDir.lastIndexOf('/')
    const label =
      relDir === '' ? '/' : slashIndex >= 0 ? relDir.slice(slashIndex + 1) : relDir

    const node: TreeNode = {
      key: dirToTreeKey(relDir),
      label,
      data: relDir,
      icon: 'pi pi-folder',
      children: [],
    }
    nodeMap.set(relDir, node)

    if (relDir !== '') {
      const parentDir = slashIndex >= 0 ? relDir.slice(0, slashIndex) : ''
      const parentNode = ensureNode(parentDir)
      parentNode.children = parentNode.children ?? []
      if (!parentNode.children.some((c) => c.key === node.key)) {
        parentNode.children.push(node)
      }
    }

    return node
  }

  for (const dir of directories) {
    ensureNode(dir)
  }

  const sortChildren = (nodes: TreeNode[] | undefined) => {
    if (!nodes || nodes.length === 0) return
    nodes.sort((a, b) => String(a.label ?? '').localeCompare(String(b.label ?? '')))
    for (const child of nodes) {
      sortChildren(child.children)
    }
  }

  const root = nodeMap.get('')
  if (root) {
    sortChildren(root.children)
    return [root]
  }

  const roots = Array.from(nodeMap.values())
  sortChildren(roots)
  return roots
}

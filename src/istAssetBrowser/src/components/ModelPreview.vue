<script setup lang="ts">
import { nextTick, onBeforeUnmount, ref, watch } from 'vue'
import Dialog from 'primevue/dialog'
import Message from 'primevue/message'
import ProgressSpinner from 'primevue/progressspinner'
import * as THREE from 'three'
import { OrbitControls } from 'three/addons/controls/OrbitControls.js'
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js'

const props = defineProps<{ fileName: string; url: string }>()
const emit = defineEmits<{ close: [] }>()
const viewport = ref<HTMLDivElement | null>(null)
const loading = ref(true)
const errorMessage = ref('')
const maximized = ref(false)
const meshStats = ref<{ vertices: number; triangles: number }>()
const countFormat = new Intl.NumberFormat('ja-JP')
const abortController = new AbortController()
let disposed = false
let started = false
let renderer: THREE.WebGLRenderer | undefined
let controls: OrbitControls | undefined
let observer: ResizeObserver | undefined
let scene: THREE.Scene | undefined

const CENTER_DURATION_MS = 200
let centering: {
  startedAt: number
  fromPosition: THREE.Vector3
  toPosition: THREE.Vector3
  fromTarget: THREE.Vector3
  toTarget: THREE.Vector3
} | undefined

const savedShadingMode = localStorage.getItem('istAssetBrowser.shadingMode')
function savedToggle(name: string, defaultValue: boolean) {
  const value = localStorage.getItem(`istAssetBrowser.preview.${name}`)
  return value === 'true' ? true : value === 'false' ? false : defaultValue
}
const showPolygons = ref(savedToggle('polygons', savedShadingMode !== 'wireframe'))
const showLines = ref(savedToggle('lines', savedShadingMode === 'wireframe' || savedShadingMode === 'combined'))
const showPoints = ref(savedToggle('points', false))
const doubleSided = ref(savedToggle('doubleSided', false))
const showTextures = ref(savedToggle('textures', true))
const shadedMeshes: {
  mesh: THREE.Mesh
  material: THREE.Material | THREE.Material[]
  lines: THREE.Mesh
  points: THREE.Points
}[] = []
const surfaceMaterials = new Map<THREE.Material, {
  side: THREE.Side
  enabled: boolean
  factor: number
  units: number
  textures: Map<string, THREE.Texture>
}>()
let hiddenMaterial: THREE.MeshBasicMaterial | undefined
let pointMaterial: THREE.PointsMaterial | undefined

function updateShadingMode() {
  if (!hiddenMaterial) return
  for (const { mesh, material, lines, points } of shadedMeshes) {
    mesh.material = showPolygons.value ? material : hiddenMaterial
    lines.visible = showLines.value
    points.visible = showPoints.value
  }
  for (const [material, original] of surfaceMaterials) {
    const combined = showPolygons.value && (showLines.value || showPoints.value)
    const side = doubleSided.value ? THREE.DoubleSide : original.side
    let changed = material.side !== side
    material.side = side
    material.polygonOffset = combined || original.enabled
    material.polygonOffsetFactor = combined ? 1 : original.factor
    material.polygonOffsetUnits = combined ? 1 : original.units
    for (const [key, texture] of original.textures) {
      const value = showTextures.value ? texture : null
      if (Reflect.get(material, key) !== value) {
        Reflect.set(material, key, value)
        changed = true
      }
    }
    if (changed) material.needsUpdate = true
  }
}

function prepareShading(model: THREE.Object3D) {
  const meshes: THREE.Mesh[] = []
  model.traverse((object) => {
    if (object instanceof THREE.Mesh) meshes.push(object)
  })
  if (meshes.length === 0) return
  hiddenMaterial = new THREE.MeshBasicMaterial({ visible: false })
  pointMaterial = new THREE.PointsMaterial({
    color: 0x000000,
    size: 3,
    sizeAttenuation: false,
    transparent: true,
    opacity: 0.05,
    depthWrite: false,
  })
  const wireMaterial = new THREE.MeshBasicMaterial({
    color: 0x000000,
    wireframe: true,
    transparent: true,
    opacity: 0.1,
    depthWrite: false,
  })
  let vertices = 0
  let triangles = 0
  for (const mesh of meshes) {
    const { geometry } = mesh
    const vertexCount = geometry.getAttribute('position').count
    const instances = mesh instanceof THREE.InstancedMesh ? mesh.count : 1
    vertices += vertexCount * instances
    triangles += Math.floor((geometry.index?.count ?? vertexCount) / 3) * instances
    const lines = new THREE.Mesh(geometry, wireMaterial)
    const points = new THREE.Points(geometry, pointMaterial)
    lines.raycast = () => {}
    points.raycast = () => {}
    mesh.add(lines, points)
    shadedMeshes.push({ mesh, material: mesh.material, lines, points })
    for (const material of Array.isArray(mesh.material) ? mesh.material : [mesh.material]) {
      if (!surfaceMaterials.has(material)) {
        const textures = new Map<string, THREE.Texture>()
        for (const [key, value] of Object.entries(material)) {
          if (value instanceof THREE.Texture) textures.set(key, value)
        }
        surfaceMaterials.set(material, {
          side: material.side,
          enabled: material.polygonOffset,
          factor: material.polygonOffsetFactor,
          units: material.polygonOffsetUnits,
          textures,
        })
      }
    }
  }
  meshStats.value = { vertices, triangles }
  updateShadingMode()
}

for (const [name, toggle] of [
  ['polygons', showPolygons], ['lines', showLines], ['points', showPoints],
  ['doubleSided', doubleSided], ['textures', showTextures],
] as const) {
  watch(toggle, (value) => {
    updateShadingMode()
    localStorage.setItem(`istAssetBrowser.preview.${name}`, String(value))
  })
}

function centerOnPolygon(event: MouseEvent) {
  if (loading.value || !showPolygons.value || !renderer || !controls || !scene) return

  const { left, top, width, height } = renderer.domElement.getBoundingClientRect()
  if (width === 0 || height === 0) return
  const pointer = new THREE.Vector2(
    ((event.clientX - left) / width) * 2 - 1,
    1 - ((event.clientY - top) / height) * 2,
  )
  const camera = controls.object
  if (!(camera instanceof THREE.PerspectiveCamera)) return
  camera.updateMatrixWorld()
  scene.updateMatrixWorld()
  const raycaster = new THREE.Raycaster()
  raycaster.setFromCamera(pointer, camera)

  const forward = camera.getWorldDirection(new THREE.Vector3())
  for (const hit of raycaster.intersectObjects(shadedMeshes.map(({ mesh }) => mesh), false)) {
    const mesh = hit.object
    if (!(mesh instanceof THREE.Mesh)) continue
    const material = Array.isArray(mesh.material) ? mesh.material[hit.face?.materialIndex ?? 0] : mesh.material
    if (!material?.visible || material.opacity === 0) continue
    let visible = true
    for (let object: THREE.Object3D | null = mesh; object; object = object.parent) {
      if (!object.visible) {
        visible = false
        break
      }
    }
    if (!visible) continue
    const depth = hit.distance * raycaster.ray.direction.dot(forward)
    if (depth < camera.near || depth > camera.far) continue

    // Pan parallel to the image plane so the picked surface keeps its apparent scale.
    centering = {
      startedAt: performance.now(),
      fromPosition: camera.position.clone(),
      toPosition: hit.point.clone().addScaledVector(forward, -depth),
      fromTarget: controls.target.clone(),
      toTarget: hit.point,
    }
    return
  }
}

function updateCentering(time: number) {
  if (!controls || !centering) return
  const progress = THREE.MathUtils.smoothstep(time - centering.startedAt, 0, CENTER_DURATION_MS)
  const camera = controls.object
  camera.position.lerpVectors(centering.fromPosition, centering.toPosition, progress)
  controls.target.lerpVectors(centering.fromTarget, centering.toTarget, progress)
  camera.lookAt(controls.target)
  if (progress === 1) centering = undefined
}

function disposeRenderer() {
  if (!renderer) return
  renderer.setAnimationLoop(null)
  renderer.dispose()
  renderer.forceContextLoss()
  renderer.domElement.remove()
  renderer = undefined
}

function disposeModel(model: THREE.Object3D) {
  const geometries = new Set<THREE.BufferGeometry>()
  const materials = new Set<THREE.Material>()
  const textures = new Set<THREE.Texture>()
  model.traverse((object) => {
    if (!(object instanceof THREE.Mesh)) return
    geometries.add(object.geometry)
    for (const material of Array.isArray(object.material) ? object.material : [object.material]) {
      materials.add(material)
      for (const value of Object.values(material)) {
        if (value instanceof THREE.Texture) textures.add(value)
      }
    }
  })
  for (const texture of textures) {
    if (typeof ImageBitmap !== 'undefined' && texture.image instanceof ImageBitmap) {
      texture.image.close()
    }
    texture.dispose()
  }
  for (const material of materials) material.dispose()
  for (const geometry of geometries) geometry.dispose()
}

function releaseViewer() {
  centering = undefined
  observer?.disconnect()
  controls?.dispose()
  for (const { mesh, material } of shadedMeshes) mesh.material = material
  for (const [material, original] of surfaceMaterials) {
    for (const [key, texture] of original.textures) Reflect.set(material, key, texture)
  }
  shadedMeshes.length = 0
  surfaceMaterials.clear()
  if (scene) {
    disposeModel(scene)
    scene.clear()
    scene = undefined
  }
  hiddenMaterial?.dispose()
  pointMaterial?.dispose()
  hiddenMaterial = undefined
  pointMaterial = undefined
  disposeRenderer()
  controls = undefined
}

function showPreviewError(error: unknown) {
  errorMessage.value = error instanceof Error ? error.message : String(error)
  releaseViewer()
}

async function initialize() {
  if (started) return
  started = true
  await nextTick()
  if (disposed || !viewport.value) return

  try {
    const host = viewport.value
    renderer = new THREE.WebGLRenderer({ antialias: true })
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
    renderer.setClearColor(0x333333)
    host.appendChild(renderer.domElement)
    const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 1000)
    scene = new THREE.Scene()
    scene.add(new THREE.HemisphereLight(0xffffff, 0x606878, 3))
    const light = new THREE.DirectionalLight(0xffffff, 3)
    light.position.set(3, 5, 4)
    scene.add(light)
    controls = new OrbitControls(camera, host)
    controls.enableDamping = true
    controls.addEventListener('start', () => { centering = undefined })

    const resize = () => {
      if (!renderer) return
      const width = Math.max(host.clientWidth, 1)
      const height = Math.max(host.clientHeight, 1)
      renderer.setSize(width, height)
      camera.aspect = width / height
      camera.updateProjectionMatrix()
    }
    observer = new ResizeObserver(resize)
    observer.observe(host)
    resize()
    renderer.setAnimationLoop((time) => {
      controls?.update()
      updateCentering(time)
      if (scene && renderer) renderer.render(scene, camera)
    })

    const response = await fetch(props.url, { signal: abortController.signal })
    if (!response.ok) {
      const detail = await response.text()
      throw new Error(`モデルの取得に失敗しました (${response.status}): ${detail}`)
    }
    const gltf = await new GLTFLoader().parseAsync(await response.arrayBuffer(), '')
    if (disposed) {
      if (gltf.scene) disposeModel(gltf.scene)
      return
    }
    if (!gltf.scene) throw new Error('プレビュー可能なメッシュがありません。')
    const model = new THREE.Group()
    model.add(gltf.scene)
    scene.add(model)
    const bounds = new THREE.Box3().setFromObject(gltf.scene)
    if (bounds.isEmpty()) throw new Error('プレビュー可能なメッシュがありません。')
    const sphere = bounds.getBoundingSphere(new THREE.Sphere())
    if (!Number.isFinite(sphere.radius) || sphere.radius <= 0) {
      throw new Error('モデルのサイズが不正です。')
    }
    gltf.scene.position.sub(sphere.center)
    model.scale.setScalar(1 / sphere.radius)
    prepareShading(gltf.scene)
    const halfFov = THREE.MathUtils.degToRad(camera.fov / 2)
    const fitAngle = Math.min(halfFov, Math.atan(Math.tan(halfFov) * camera.aspect))
    const distance = 1.15 / Math.sin(fitAngle)
    camera.position.set(1, 0.7, 1).normalize().multiplyScalar(distance)
    camera.far = Math.max(1000, distance * 10)
    camera.updateProjectionMatrix()
    controls.target.set(0, 0, 0)
    controls.update()
  } catch (error) {
    if (!disposed) showPreviewError(error)
  } finally {
    loading.value = false
  }
}

onBeforeUnmount(() => {
  disposed = true
  abortController.abort()
  releaseViewer()
})
</script>

<template>
  <Dialog
    :visible="true"
    :header="fileName"
    modal
    maximizable
    :dismissableMask="!maximized"
    :style="{
      width: maximized ? '100vw' : '90vw',
      maxWidth: maximized ? 'none' : '1400px',
      height: maximized ? '100vh' : '85vh',
    }"
    :contentStyle="{ display: 'flex', flexDirection: 'column', flex: '1', minHeight: '0', overflow: 'hidden' }"
    @maximize="maximized = true"
    @unmaximize="maximized = false"
    @update:visible="(visible) => { if (!visible) emit('close') }"
    @show="initialize"
  >
    <div class="shading-toolbar" role="group" aria-label="プレビュー表示設定">
      <button type="button" class="preview-toggle" :class="{ active: showPolygons }" :aria-pressed="showPolygons" aria-label="ポリゴン" title="ポリゴン" :disabled="loading || !!errorMessage" @click="showPolygons = !showPolygons">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3 22 20H2Z" fill="currentColor" /></svg>
      </button>
      <button type="button" class="preview-toggle" :class="{ active: showLines }" :aria-pressed="showLines" aria-label="ワイヤフレーム" title="ワイヤフレーム" :disabled="loading || !!errorMessage" @click="showLines = !showLines">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3 22 20H2Z" fill="none" stroke="currentColor" stroke-width="2" /></svg>
      </button>
      <button type="button" class="preview-toggle" :class="{ active: showPoints }" :aria-pressed="showPoints" aria-label="ポイント" title="ポイント" :disabled="loading || !!errorMessage" @click="showPoints = !showPoints">
        <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="4" r="2.5" /><circle cx="4" cy="19" r="2.5" /><circle cx="20" cy="19" r="2.5" /></svg>
      </button>
      <button type="button" class="preview-toggle" :class="{ active: doubleSided }" :aria-pressed="doubleSided" aria-label="両面描画" title="両面描画" :disabled="loading || !!errorMessage" @click="doubleSided = !doubleSided">
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <path d="M3 3h18v18H3z" transform="translate(2 -2)" fill="none" stroke="currentColor" stroke-width="2" />
          <path d="M3 3h18v18H3z" transform="translate(-2 2)" fill="currentColor" />
        </svg>
      </button>
      <button type="button" class="preview-toggle" :class="{ active: showTextures }" :aria-pressed="showTextures" aria-label="テクスチャ" title="テクスチャ" :disabled="loading || !!errorMessage" @click="showTextures = !showTextures">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 3h18v18H3z" fill="none" stroke="currentColor" stroke-width="2" /><path d="M4 4h8v8H4zm8 8h8v8h-8z" fill="currentColor" /></svg>
      </button>
    </div>
    <div class="preview-shell">
      <div ref="viewport" class="preview-viewport" aria-label="3D モデルプレビュー" @dblclick="centerOnPolygon" />
      <div v-if="loading" class="preview-overlay" role="status">
        <ProgressSpinner style="width: 40px; height: 40px" strokeWidth="4" />
        <span>モデルを変換・読み込み中...</span>
      </div>
      <div v-else-if="errorMessage" class="preview-overlay">
        <Message severity="error" :closable="false">{{ errorMessage }}</Message>
      </div>
    </div>
    <div class="preview-footer">
      <span class="preview-help">ドラッグ: 回転 / ホイール: ズーム / 右ドラッグ: 移動 / ダブルクリック: 中心に移動</span>
      <div v-if="!loading && !errorMessage && meshStats" class="preview-stats">
        <span>頂点数: {{ countFormat.format(meshStats.vertices) }}</span>
        <span>ポリゴン数: {{ countFormat.format(meshStats.triangles) }}</span>
      </div>
    </div>
  </Dialog>
</template>

<style scoped>
.shading-toolbar {
  display: flex;
  flex-shrink: 0;
  flex-wrap: wrap;
  gap: 8px;
  margin-bottom: 12px;
}

.preview-toggle {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  border: 1px solid var(--p-content-border-color);
  border-radius: 6px;
  background: var(--p-content-background);
  color: var(--p-text-muted-color);
  cursor: pointer;
}

.preview-toggle.active {
  background: #add8e6;
  border-color: #add8e6;
  color: #172a32;
}

.preview-toggle:focus-visible {
  outline: none;
  filter: brightness(0.85);
}

.preview-toggle:disabled {
  opacity: 0.5;
  cursor: default;
}

.preview-toggle svg {
  width: 20px;
  height: 20px;
  fill: currentColor;
}

.preview-shell {
  position: relative;
  flex: 1;
  min-height: 0;
}

.preview-viewport {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  overflow: hidden;
  background: #333333;
}

.preview-viewport :deep(canvas) {
  display: block;
}

.preview-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 12px;
  padding: 16px;
  overflow: auto;
}

.preview-footer {
  display: flex;
  flex-shrink: 0;
  flex-wrap: wrap;
  justify-content: space-between;
  gap: 4px 16px;
  margin-top: 8px;
  font-size: 0.8rem;
}

.preview-help {
  color: #9ca3af;
}

.preview-stats {
  display: flex;
  flex-wrap: wrap;
  gap: 4px 12px;
}
</style>

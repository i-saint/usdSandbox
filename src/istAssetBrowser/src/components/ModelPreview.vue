<script setup lang="ts">
import { nextTick, onBeforeUnmount, ref, watch } from 'vue'
import Select from 'primevue/select'
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
const abortController = new AbortController()
let disposed = false
let started = false
let renderer: THREE.WebGLRenderer | undefined
let controls: OrbitControls | undefined
let observer: ResizeObserver | undefined
let scene: THREE.Scene | undefined

type ShadingMode = 'solid' | 'wireframe' | 'combined'
const STORAGE_KEY_SHADING_MODE = 'istAssetBrowser.shadingMode'
const savedShadingMode = localStorage.getItem(STORAGE_KEY_SHADING_MODE)
const shadingMode = ref<ShadingMode>(
  savedShadingMode === 'wireframe' || savedShadingMode === 'combined' ? savedShadingMode : 'solid',
)
const shadingModes: { value: ShadingMode; label: string }[] = [
  { value: 'solid', label: 'ポリゴン' },
  { value: 'wireframe', label: 'ワイヤフレーム' },
  { value: 'combined', label: 'ポリゴン＋ワイヤフレーム' },
]
const shadedMeshes: {
  mesh: THREE.Mesh
  material: THREE.Material | THREE.Material[]
  overlay: THREE.Mesh
}[] = []
const surfaceMaterials = new Map<THREE.Material, { enabled: boolean; factor: number; units: number }>()

function updateShadingMode() {
  for (const { mesh, material, overlay } of shadedMeshes) {
    mesh.material = shadingMode.value === 'wireframe' ? overlay.material : material
    overlay.visible = shadingMode.value === 'combined'
  }
  for (const [material, original] of surfaceMaterials) {
    const combined = shadingMode.value === 'combined'
    material.polygonOffset = combined || original.enabled
    material.polygonOffsetFactor = combined ? 1 : original.factor
    material.polygonOffsetUnits = combined ? 1 : original.units
  }
}

function prepareShading(model: THREE.Object3D) {
  const meshes: THREE.Mesh[] = []
  model.traverse((object) => {
    if (object instanceof THREE.Mesh) meshes.push(object)
  })
  if (meshes.length === 0) return
  const wireMaterial = new THREE.MeshBasicMaterial({ color: 0x60a5fa, wireframe: true })
  for (const mesh of meshes) {
    const overlay = new THREE.Mesh(mesh.geometry, wireMaterial)
    overlay.visible = false
    overlay.raycast = () => {}
    mesh.add(overlay)
    shadedMeshes.push({ mesh, material: mesh.material, overlay })
    for (const material of Array.isArray(mesh.material) ? mesh.material : [mesh.material]) {
      if (!surfaceMaterials.has(material)) {
        surfaceMaterials.set(material, {
          enabled: material.polygonOffset,
          factor: material.polygonOffsetFactor,
          units: material.polygonOffsetUnits,
        })
      }
    }
  }
  updateShadingMode()
}

watch(shadingMode, (mode) => {
  updateShadingMode()
  localStorage.setItem(STORAGE_KEY_SHADING_MODE, mode)
})

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
  observer?.disconnect()
  renderer?.setAnimationLoop(null)
  controls?.dispose()
  for (const { mesh, material } of shadedMeshes) mesh.material = material
  shadedMeshes.length = 0
  surfaceMaterials.clear()
  if (scene) {
    disposeModel(scene)
    scene.clear()
    scene = undefined
  }
  renderer?.dispose()
  renderer?.forceContextLoss()
  renderer?.domElement.remove()
  renderer = undefined
  controls = undefined
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
    renderer.setClearColor(0x14161a)
    host.appendChild(renderer.domElement)
    const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 1000)
    scene = new THREE.Scene()
    scene.add(new THREE.HemisphereLight(0xffffff, 0x606878, 3))
    const light = new THREE.DirectionalLight(0xffffff, 3)
    light.position.set(3, 5, 4)
    scene.add(light)
    controls = new OrbitControls(camera, renderer.domElement)
    controls.enableDamping = true

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
    renderer.setAnimationLoop(() => {
      controls?.update()
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
    if (!disposed) {
      errorMessage.value = error instanceof Error ? error.message : String(error)
      releaseViewer()
    }
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
    <div class="shading-toolbar">
      <Select
        v-model="shadingMode"
        :options="shadingModes"
        optionLabel="label"
        optionValue="value"
        aria-label="シェーディングモード"
        :disabled="loading || !!errorMessage"
        size="small"
      />
    </div>
    <div class="preview-shell">
      <div ref="viewport" class="preview-viewport" aria-label="3D モデルプレビュー" />
      <div v-if="loading" class="preview-overlay" role="status">
        <ProgressSpinner style="width: 40px; height: 40px" strokeWidth="4" />
        <span>モデルを変換・読み込み中...</span>
      </div>
      <div v-else-if="errorMessage" class="preview-overlay">
        <Message severity="error" :closable="false">{{ errorMessage }}</Message>
      </div>
    </div>
    <div class="preview-help">ドラッグ: 回転 / ホイール: ズーム / 右ドラッグ: 移動</div>
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
  background: #14161a;
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

.preview-help {
  flex-shrink: 0;
  margin-top: 8px;
  font-size: 0.8rem;
  color: #9ca3af;
}
</style>

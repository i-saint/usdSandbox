<script setup lang="ts">
import { nextTick, onBeforeUnmount, ref } from 'vue'
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
const abortController = new AbortController()
let disposed = false
let started = false
let renderer: THREE.WebGLRenderer | undefined
let controls: OrbitControls | undefined
let observer: ResizeObserver | undefined
let scene: THREE.Scene | undefined

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
    :style="{ width: '90vw', maxWidth: '1400px' }"
    @update:visible="(visible) => { if (!visible) emit('close') }"
    @show="initialize"
  >
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
.preview-shell {
  position: relative;
}

.preview-viewport {
  width: 100%;
  height: 65vh;
  min-height: 240px;
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
  margin-top: 8px;
  font-size: 0.8rem;
  color: #9ca3af;
}
</style>

<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import ProgressSpinner from 'primevue/progressspinner'
import Tag from 'primevue/tag'

const props = defineProps<{
  fileName: string
  screenshotUrl: string
  cardWidth: number
}>()

const imageState = ref<'loading' | 'loaded' | 'error'>('loading')

watch(
  () => props.screenshotUrl,
  () => {
    imageState.value = 'loading'
  },
)

const extensionLabel = computed(() => {
  const dotIdx = props.fileName.lastIndexOf('.')
  return dotIdx >= 0 ? props.fileName.slice(dotIdx + 1).toUpperCase() : 'ASSET'
})

const tagSeverity = computed(() => {
  const ext = extensionLabel.value.toLowerCase()
  if (ext === 'fbx') return 'warn'
  if (ext.startsWith('usd')) return 'info'
  return 'secondary'
})

function onLoad() {
  imageState.value = 'loaded'
}

function onError() {
  imageState.value = 'error'
}
</script>

<template>
  <div class="asset-card" :style="{ width: `${cardWidth}px` }" :title="fileName">
    <div class="thumb-box" :style="{ height: `${Math.round(cardWidth * 0.75)}px` }">
      <div v-if="imageState === 'loading'" class="thumb-placeholder">
        <ProgressSpinner
          style="width: 28px; height: 28px"
          strokeWidth="4"
          animationDuration=".8s"
        />
      </div>

      <div v-else-if="imageState === 'error'" class="thumb-placeholder thumb-error">
        <i class="pi pi-box" style="font-size: 2rem" />
        <span>No Preview</span>
      </div>

      <img
        v-show="imageState === 'loaded'"
        :src="screenshotUrl"
        :alt="fileName"
        class="thumb-img"
        loading="lazy"
        @load="onLoad"
        @error="onError"
      />

      <Tag :value="extensionLabel" :severity="tagSeverity" class="ext-badge" />
    </div>

    <div class="asset-footer">
      <span class="asset-name">{{ fileName }}</span>
    </div>
  </div>
</template>

<style scoped>
.asset-card {
  display: flex;
  flex-direction: column;
  background: #1c1f24;
  border: 1px solid #2c313a;
  border-radius: 8px;
  overflow: hidden;
  transition:
    border-color 0.15s ease,
    transform 0.15s ease;
}

.asset-card:hover {
  border-color: #60a5fa;
  transform: translateY(-2px);
}

.thumb-box {
  position: relative;
  width: 100%;
  background: #14161a;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
}

.thumb-img {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: block;
}

.thumb-placeholder {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 6px;
  color: #6b7280;
  font-size: 0.78rem;
}

.ext-badge {
  position: absolute;
  top: 6px;
  right: 6px;
  font-size: 0.68rem;
  padding: 2px 6px;
  opacity: 0.92;
}

.asset-footer {
  padding: 8px 10px;
  border-top: 1px solid #262a32;
}

.asset-name {
  display: block;
  font-size: 0.84rem;
  color: #e5e7eb;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
</style>

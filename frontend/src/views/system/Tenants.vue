<script setup lang="ts">
import { computed, reactive, ref } from 'vue'
import { Field, Switch, Tag, TextInput, type Column } from '@/components/ui'
import { DictSelect, FormDialog, ListPage, PermissionButton } from '@/components/business'
import { systemApi, type TenantRow } from '@/api'
import { useListPage } from '@/composables/list'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'
import { formatLocalDateTime } from '@/utils/time'
import { pattern, required, validate } from '@/utils/validate'

const list = useListPage<TenantRow, { keyword: string; status: string }>({
  query: { keyword: '', status: '' },
  fetch: params => systemApi.tenants.list(params),
})

const columns: Column[] = [
  { key: 'name', title: '租户名称', minWidth: 180 },
  { key: 'code', title: '租户编码' },
  { key: 'status', title: '状态', width: 96 },
  { key: 'remark', title: '备注' },
  { key: 'createTime', title: '创建时间', width: 150 },
  { key: 'actions', title: '操作', width: 150, className: 'col-actions' },
]

const isDefault = (row: { id?: number }) => row.id === 1

interface TenantForm {
  id?: number
  code: string
  name: string
  remark: string
  adminUsername: string
  adminNickname: string
  adminPassword: string
  status: number
}
const emptyForm = (): TenantForm => ({ code: '', name: '', remark: '', adminUsername: '', adminNickname: '', adminPassword: '', status: 1 })
const dialog = ref(false)
const saving = ref(false)
const form = reactive<TenantForm>(emptyForm())
const errors = ref<Partial<Record<keyof TenantForm, string>>>({})
const isEdit = computed(() => Boolean(form.id))

function openCreate() {
  Object.assign(form, emptyForm(), { id: undefined })
  errors.value = {}
  dialog.value = true
}

function openEdit(row: TenantRow) {
  Object.assign(form, { id: row.id, code: row.code, name: row.name, remark: row.remark, adminUsername: '', adminNickname: '', adminPassword: '', status: row.status })
  errors.value = {}
  dialog.value = true
}

async function submit() {
  errors.value = validate(form, {
    name: [required('请输入租户名称')],
    code: isEdit.value ? [] : [required('请输入租户编码'), pattern(/^[a-z][a-z0-9_]{0,31}$/, '小写字母开头，仅含小写字母、数字与下划线，最长 32 位')],
    adminUsername: isEdit.value ? [] : [required('请输入管理员登录名')],
    adminPassword: isEdit.value ? [] : [required('请输入管理员初始密码'), v => (String(v).length < 8 ? '密码至少 8 位（服务端策略可能更严）' : undefined)],
  })
  if (Object.keys(errors.value).length) return
  saving.value = true
  try {
    if (form.id) {
      await systemApi.tenants.update(form.id, { name: form.name.trim(), remark: form.remark, status: form.status })
      pushToast('租户已更新')
    } else {
      await systemApi.tenants.create({
        code: form.code.trim(),
        name: form.name.trim(),
        remark: form.remark,
        adminUsername: form.adminUsername.trim(),
        adminNickname: form.adminNickname.trim() || undefined,
        adminPassword: form.adminPassword,
      })
      pushToast('租户已创建，管理员账号可立即登录')
    }
    dialog.value = false
    await list.load()
  } catch (error) {
    toastError(error, '租户保存失败')
  } finally {
    saving.value = false
  }
}

async function toggleStatus(row: TenantRow) {
  const next = row.status === 1 ? 0 : 1
  if (next === 0) {
    const ok = await confirm({ title: '停用租户', message: `确认停用「${row.name}」吗？停用后该租户下所有账号将无法登录。`, confirmText: '停用', danger: true })
    if (!ok) return
  }
  try {
    await systemApi.tenants.update(row.id, { status: next })
    pushToast(`已${next === 1 ? '启用' : '停用'}「${row.name}」`)
    await list.load()
  } catch (error) {
    toastError(error, '租户状态更新失败')
  }
}
</script>

<template>
  <ListPage title="租户管理" desc="按租户隔离账号与设备数据；停用租户会立即阻止其所有账号登录。" table-title="租户列表" :list="list" :columns="columns" :min-width="900">
    <template #actions>
      <PermissionButton perm="system:tenant:add" variant="primary" icon="plus" @click="openCreate">新增租户</PermissionButton>
    </template>

    <template #search>
      <Field label="关键词" wide>
        <TextInput v-model="list.query.keyword" icon="search" placeholder="租户名称 / 编码" clearable @enter="list.search()" />
      </Field>
      <Field label="状态">
        <DictSelect v-model="list.query.status" type="sys_normal_disable" all-label="全部状态" />
      </Field>
    </template>

    <template #cell-name="{ row }">
      <div class="cell-main">
        <div style="min-width: 0">
          <div class="cell-title ellipsis">{{ row.name }}<Tag v-if="isDefault(row)" tone="info" style="margin-left: 8px">默认</Tag></div>
          <div class="cell-sub mono">#{{ row.id }}</div>
        </div>
      </div>
    </template>
    <template #cell-code="{ row }"><span class="mono">{{ row.code }}</span></template>
    <template #cell-status="{ row }"><Tag :tone="row.status === 1 ? 'success' : 'muted'">{{ row.status === 1 ? '启用' : '停用' }}</Tag></template>
    <template #cell-remark="{ row }"><span class="muted">{{ row.remark || '—' }}</span></template>
    <template #cell-createTime="{ row }"><span class="tnum soft">{{ formatLocalDateTime(row.createTime) }}</span></template>
    <template #cell-actions="{ row }">
      <div class="row-actions">
        <PermissionButton perm="system:tenant:edit" icon-only icon="edit" label="编辑" @click="openEdit(row)" />
        <PermissionButton
          v-if="!isDefault(row)"
          :perm="row.status === 1 ? 'system:tenant:remove' : 'system:tenant:edit'"
          icon-only
          icon="power"
          :label="row.status === 1 ? '停用' : '启用'"
          :danger="row.status === 1"
          @click="toggleStatus(row)"
        />
      </div>
    </template>

    <FormDialog :open="dialog" :title="isEdit ? '编辑租户' : '新增租户'" :saving="saving" @close="dialog = false" @submit="submit">
      <Field label="租户编码" required :error="errors.code" :hint="isEdit ? '编码创建后不可变更，登录时用它区分租户' : '登录时填写；小写字母开头，最长 32 位'">
        <TextInput v-model="form.code" placeholder="例如 acme" :disabled="isEdit" :invalid="!!errors.code" autocomplete="off" />
      </Field>
      <Field label="租户名称" required :error="errors.name">
        <TextInput v-model="form.name" placeholder="例如 示范工厂" :invalid="!!errors.name" />
      </Field>
      <template v-if="!isEdit">
        <Field label="管理员登录名" required :error="errors.adminUsername">
          <TextInput v-model="form.adminUsername" placeholder="租户首个管理员账号" :invalid="!!errors.adminUsername" autocomplete="off" />
        </Field>
        <Field label="管理员姓名">
          <TextInput v-model="form.adminNickname" placeholder="默认与登录名相同" />
        </Field>
        <Field label="管理员初始密码" required :error="errors.adminPassword" span>
          <TextInput v-model="form.adminPassword" type="password" placeholder="至少 8 位（服务端策略可能更严）" :invalid="!!errors.adminPassword" autocomplete="new-password" />
        </Field>
      </template>
      <Field label="备注" span>
        <TextInput v-model="form.remark" placeholder="选填" />
      </Field>
      <Field v-if="isEdit && !isDefault(form)" label="租户状态" group>
        <Switch :model-value="form.status === 1" :label="form.status === 1 ? '启用，允许登录' : '停用，禁止登录'" @update:model-value="form.status = $event ? 1 : 0" />
      </Field>
    </FormDialog>
  </ListPage>
</template>

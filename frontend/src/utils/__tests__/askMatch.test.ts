import test from 'node:test'
import assert from 'node:assert/strict'
import { permissionFromQuestion } from '../askMatch.ts'

test('permissionFromQuestion maps reset-password questions to the seeded reset code', () => {
  assert.equal(permissionFromQuestion('用户可以重置密码吗'), 'system:user:reset')
})

test('permissionFromQuestion combines verb and object for other admin questions', () => {
  assert.equal(permissionFromQuestion('谁可以删除用户'), 'system:user:remove')
  assert.equal(permissionFromQuestion('谁能新增角色'), 'system:role:add')
  assert.equal(permissionFromQuestion('谁可以导出操作日志'), 'monitor:operlog:list')
})

test('permissionFromQuestion returns an explicit code or null when nothing matches', () => {
  assert.equal(permissionFromQuestion('谁有 system:dict:edit'), 'system:dict:edit')
  assert.equal(permissionFromQuestion('谁能看'), null)
})

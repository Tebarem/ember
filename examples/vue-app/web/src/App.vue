<script setup>
import { ref } from 'vue';
import { useMutation, useQuery, useQueryClient } from '@tanstack/vue-query';
import { addNote, deleteNote, getNotes, getSystem } from './api.js';

const queryClient = useQueryClient();
const system = useQuery({ queryKey: ['system'], queryFn: getSystem });
const notes = useQuery({ queryKey: ['notes'], queryFn: getNotes });

const refresh = () => queryClient.invalidateQueries({ queryKey: ['notes'] });
const add = useMutation({ mutationFn: addNote, onSuccess: refresh });
const remove = useMutation({ mutationFn: deleteNote, onSuccess: refresh });

const draft = ref('');
function submit() {
  const text = draft.value.trim();
  if (text === '') return;
  add.mutate(text, { onSuccess: () => (draft.value = '') });
}
</script>

<template>
  <main>
    <header>
      <h1>Notes</h1>
      <p v-if="system.data.value" class="system">
        {{ system.data.value.platform }} · {{ system.data.value.cpus }} cores ·
        {{ system.data.value.ramMB }} MB RAM · Ember {{ system.data.value.ember }}
      </p>
    </header>

    <form @submit.prevent="submit">
      <input v-model="draft" placeholder="Write a note and press Enter" autofocus />
      <button type="submit" :disabled="add.isPending.value">Add</button>
    </form>

    <p v-if="notes.isPending.value" class="muted">Loading…</p>
    <p v-else-if="notes.isError.value" class="error">{{ notes.error.value.message }}</p>
    <p v-else-if="notes.data.value.length === 0" class="muted">No notes yet.</p>
    <ul v-else>
      <li v-for="note in notes.data.value" :key="note.id">
        <span>{{ note.text }}</span>
        <button class="delete" title="Delete" @click="remove.mutate(note.id)">×</button>
      </li>
    </ul>

    <footer class="muted">
      Saved by the host in {{ system.data.value?.notesFile ?? '…' }}
      <span v-if="notes.isFetching.value"> · syncing</span>
    </footer>
  </main>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';

const STORAGE_KEY = 'todos-vue';

function loadTodos() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? JSON.parse(raw) : [];
  } catch {
    return [];
  }
}

const todos = ref(loadTodos());
const newTitle = ref('');
const editingId = ref(null);
const editTitle = ref('');
const filter = ref('all');
let nextSeq = 0;

function readFilter() {
  const name = location.hash.slice(2);
  filter.value = name === 'active' || name === 'completed' ? name : 'all';
}

watch(
  todos,
  (value) => {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(value));
    } catch {
      // Storage can be unavailable; the list still works in memory.
    }
  },
  { deep: true },
);

onMounted(() => {
  readFilter();
  window.addEventListener('hashchange', readFilter);
});
onBeforeUnmount(() => window.removeEventListener('hashchange', readFilter));

const visible = computed(() =>
  todos.value.filter((t) =>
    filter.value === 'all' ? true : filter.value === 'active' ? !t.completed : t.completed,
  ),
);
const activeCount = computed(() => todos.value.filter((t) => !t.completed).length);
const allDone = computed(() => todos.value.length > 0 && activeCount.value === 0);

function addTodo() {
  const title = newTitle.value.trim();
  if (!title) return;
  nextSeq += 1;
  todos.value.push({ id: `${Date.now()}-${nextSeq}`, title, completed: false });
  newTitle.value = '';
}

function startEdit(todo) {
  editingId.value = todo.id;
  editTitle.value = todo.title;
  nextTick(() => {
    const input = document.querySelector('.todo-list li.editing .edit');
    if (input) input.focus();
  });
}

function remove(todo) {
  todos.value = todos.value.filter((t) => t !== todo);
}

function commitEdit(todo) {
  if (editingId.value !== todo.id) return;
  const title = editTitle.value.trim();
  editingId.value = null;
  if (!title) remove(todo);
  else todo.title = title;
}

function cancelEdit() {
  editingId.value = null;
}

function toggleAll() {
  const done = !allDone.value;
  todos.value.forEach((t) => (t.completed = done));
}

function clearCompleted() {
  todos.value = todos.value.filter((t) => !t.completed);
}
</script>

<template>
  <section class="todoapp">
    <header class="header">
      <h1>todos</h1>
      <input
        class="new-todo"
        placeholder="What needs to be done?"
        autofocus
        v-model="newTitle"
        @keydown.enter="addTodo"
      />
    </header>
    <section class="main" v-if="todos.length > 0">
      <input id="toggle-all" class="toggle-all" type="checkbox" :checked="allDone" @change="toggleAll" />
      <label for="toggle-all">Mark all as complete</label>
      <ul class="todo-list">
        <li
          v-for="todo in visible"
          :key="todo.id"
          :class="{ completed: todo.completed, editing: editingId === todo.id }"
        >
          <div class="view">
            <input class="toggle" type="checkbox" v-model="todo.completed" />
            <label @dblclick="startEdit(todo)">{{ todo.title }}</label>
            <button class="destroy" aria-label="Delete" @click="remove(todo)"></button>
          </div>
          <input
            v-if="editingId === todo.id"
            class="edit"
            v-model="editTitle"
            @blur="commitEdit(todo)"
            @keydown.enter="commitEdit(todo)"
            @keydown.esc="cancelEdit"
          />
        </li>
      </ul>
    </section>
    <footer class="footer" v-if="todos.length > 0">
      <span class="todo-count">
        <strong>{{ activeCount }}</strong> {{ activeCount === 1 ? 'item' : 'items' }} left
      </span>
      <ul class="filters">
        <li><a :class="{ selected: filter === 'all' }" href="#/">All</a></li>
        <li><a :class="{ selected: filter === 'active' }" href="#/active">Active</a></li>
        <li><a :class="{ selected: filter === 'completed' }" href="#/completed">Completed</a></li>
      </ul>
      <button v-if="todos.length > activeCount" class="clear-completed" @click="clearCompleted">
        Clear completed
      </button>
      <span v-else></span>
    </footer>
  </section>
</template>

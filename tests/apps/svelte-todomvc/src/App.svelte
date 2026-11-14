<script module>
  export function focusOnMount(node) {
    node.focus();
  }
</script>

<script>
  import { fade, slide } from 'svelte/transition';

  const STORAGE_KEY = 'todos-svelte';

  function loadTodos() {
    try {
      const raw = localStorage.getItem(STORAGE_KEY);
      return raw ? JSON.parse(raw) : [];
    } catch {
      return [];
    }
  }

  let todos = $state(loadTodos());
  let newTitle = $state('');
  let editingId = $state(null);
  let editTitle = $state('');
  let filter = $state('all');
  let seq = 0;

  const visible = $derived(
    todos.filter((t) => (filter === 'all' ? true : filter === 'active' ? !t.completed : t.completed)),
  );
  const activeCount = $derived(todos.filter((t) => !t.completed).length);
  const allDone = $derived(todos.length > 0 && activeCount === 0);

  $effect(() => {
    const snapshot = JSON.stringify(todos);
    try {
      localStorage.setItem(STORAGE_KEY, snapshot);
    } catch {
      // Storage can be unavailable; the list still works in memory.
    }
  });

  function readFilter() {
    const name = location.hash.slice(2);
    filter = name === 'active' || name === 'completed' ? name : 'all';
  }

  $effect(() => {
    readFilter();
    window.addEventListener('hashchange', readFilter);
    return () => window.removeEventListener('hashchange', readFilter);
  });

  function addTodo(event) {
    if (event.key !== 'Enter') return;
    const title = newTitle.trim();
    if (!title) return;
    seq += 1;
    todos.push({ id: `${Date.now()}-${seq}`, title, completed: false });
    newTitle = '';
  }

  function startEdit(todo) {
    editingId = todo.id;
    editTitle = todo.title;
  }

  function remove(todo) {
    todos = todos.filter((t) => t !== todo);
  }

  function commitEdit(todo) {
    if (editingId !== todo.id) return;
    const title = editTitle.trim();
    editingId = null;
    if (!title) remove(todo);
    else todo.title = title;
  }

  function onEditKey(event, todo) {
    if (event.key === 'Enter') commitEdit(todo);
    else if (event.key === 'Escape') editingId = null;
  }

  function toggleAll() {
    const done = !allDone;
    for (const t of todos) t.completed = done;
  }

  function clearCompleted() {
    todos = todos.filter((t) => !t.completed);
  }
</script>

<section class="todoapp">
  <header class="header">
    <h1>todos</h1>
    <input
      class="new-todo"
      placeholder="What needs to be done?"
      autofocus
      bind:value={newTitle}
      onkeydown={addTodo}
    />
  </header>
  {#if todos.length > 0}
    <section class="main">
      <input id="toggle-all" class="toggle-all" type="checkbox" checked={allDone} onchange={toggleAll} />
      <label for="toggle-all">Mark all as complete</label>
      <ul class="todo-list">
        {#each visible as todo (todo.id)}
          <li class:completed={todo.completed} class:editing={editingId === todo.id} transition:slide={{ duration: 150 }}>
            <div class="view">
              <input class="toggle" type="checkbox" bind:checked={todo.completed} />
              <label ondblclick={() => startEdit(todo)} transition:fade={{ duration: 120 }}>{todo.title}</label>
              <button class="destroy" aria-label="Delete" onclick={() => remove(todo)}></button>
            </div>
            {#if editingId === todo.id}
              <input
                class="edit"
                bind:value={editTitle}
                onblur={() => commitEdit(todo)}
                onkeydown={(e) => onEditKey(e, todo)}
                use:focusOnMount
              />
            {/if}
          </li>
        {/each}
      </ul>
    </section>
    <footer class="footer">
      <span class="todo-count">
        <strong>{activeCount}</strong> {activeCount === 1 ? 'item' : 'items'} left
      </span>
      <ul class="filters">
        <li><a class:selected={filter === 'all'} href="#/">All</a></li>
        <li><a class:selected={filter === 'active'} href="#/active">Active</a></li>
        <li><a class:selected={filter === 'completed'} href="#/completed">Completed</a></li>
      </ul>
      {#if todos.length > activeCount}
        <button class="clear-completed" onclick={clearCompleted}>Clear completed</button>
      {:else}
        <span></span>
      {/if}
    </footer>
  {/if}
</section>

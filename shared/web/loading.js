// Keep the initial page hidden until its settings, status and inputs are ready.
async function finishPageLoading(initialLoad) {
  let timer;
  try {
    await Promise.race([initialLoad, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('The device took too long to respond.')), 15000);
    })]);
    document.body.classList.remove('page-loading');
    document.body.setAttribute('aria-busy', 'false');
    document.getElementById('pageLoader').remove();
  } catch (error) {
    const loader = document.getElementById('pageLoader');
    loader.classList.add('has-error');
    document.body.setAttribute('aria-busy', 'false');
    document.getElementById('pageLoaderMessage').textContent = 'Cannot load the page: ' + error.message;
    const retry = document.getElementById('pageLoaderRetry');
    retry.hidden = false;
    retry.onclick = () => location.reload();
    retry.focus();
  } finally {clearTimeout(timer)}
}

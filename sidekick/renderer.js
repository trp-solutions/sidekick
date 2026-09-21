const form = document.querySelector('form');
const message = document.querySelector('#message');
window.settings.get().then(config => {
    form.elements.configUrl.value = config.configUrl;
    form.elements.serialPort.value = config.serialPort;
}).catch(error => { message.textContent = error.message; });
form.addEventListener('submit', async event => {
    event.preventDefault();
    form.querySelector('button').disabled = true;
    try {
        await window.settings.save({ configUrl: form.elements.configUrl.value, serialPort: form.elements.serialPort.value });
        message.textContent = 'Saved. Reconnecting…';
    } catch (error) { message.textContent = error.message; }
    finally { form.querySelector('button').disabled = false; }
});

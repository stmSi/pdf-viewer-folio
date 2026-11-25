Module['folioWrite'] = (path, data, name, locator) => {
    FS.writeFile(path, data, { canOwn: true });
    Module['ccall']('folio_web_file', null, ['string', 'string', 'string', 'string'], [path, name, '', locator]);
};

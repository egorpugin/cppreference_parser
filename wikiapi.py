import pywikibot
import sys
from concurrent.futures import *

site = pywikibot.Site('mywiki', 'mywiki')

i = 0

def make_page(n, page_name, fn):
    global i
    i = i + 1
    if i < n:
        return
    print(f'[{n}] processing {page_name} {fn}')
    page = pywikibot.Page(site, page_name)
    page.text = open(fn, mode='r', encoding='utf-8').read()
    page.save("upload")

